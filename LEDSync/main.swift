// LED Sync - a small menu bar app: the ESP32 LED strip follows the colors at the edges of your screen.
// No third-party code. Uses ScreenCaptureKit (screen), POSIX termios (USB serial) and AppKit (menu).
//
// Build:  ./build.sh        (needs Xcode or the Command Line Tools: xcode-select --install)
// Check:  "LED Sync.app/Contents/MacOS/LEDSync" --selftest

import Cocoa
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import ServiceManagement

// =====================================================================================
//  SETTINGS YOU MAY WANT TO CHANGE
// =====================================================================================
let serialBaud: Int = 921600          // must match SYNC_BAUD in the sketch
let captureWidth: Int = 640           // the screen is captured shrunk to this width (smaller = lighter)
let targetFPS: Int = 60
let edgeDepth: Double = 0.06          // how deep into the screen to look (0.06 = outer 6 percent)
let blackCutoff: Float = 10           // colors dimmer than this become fully off

let sideNames = ["top", "right", "bottom", "left"]       // clockwise; "natural" direction: top/bottom left->right, left/right top->bottom

// =====================================================================================
//  MAPPING (same model as ambilight.py)
// =====================================================================================
struct Segment {
    var first: Int
    var last: Int
    var side: Int            // index into sideNames
    var reverse: Bool
    var from: Double
    var to: Double
}

struct Mapping {
    var numLeds: Int
    var segs: [Segment]
}

struct MapError: Error { let msg: String }

// Your mapping (outer lap = LEDs 0-87, inner lap = 88-149). Written to mapping.json the first time the app runs.
let defaultMappingJSON = """
{
  "numLeds": 150,
  "segments": [
    { "first": 0,   "last": 21,  "side": "bottom", "direction": "forward", "from": 0.1154, "to": 1.0 },
    { "first": 22,  "last": 38,  "side": "right",  "direction": "reverse" },
    { "first": 39,  "last": 69,  "side": "top",    "direction": "reverse" },
    { "first": 70,  "last": 83,  "side": "left",   "direction": "forward" },
    { "first": 84,  "last": 87,  "side": "bottom", "direction": "forward", "from": 0.0, "to": 0.1154 },
    { "first": 88,  "last": 89,  "side": "bottom", "direction": "reverse", "from": 0.0, "to": 0.1333 },
    { "first": 90,  "last": 101, "side": "left",   "direction": "reverse" },
    { "first": 102, "last": 126, "side": "top",    "direction": "forward" },
    { "first": 127, "last": 135, "side": "right",  "direction": "forward" },
    { "first": 136, "last": 149, "side": "bottom", "direction": "reverse", "from": 0.1333, "to": 1.0 }
  ]
}
"""

func parseMapping(_ data: Data) throws -> Mapping {
    let obj: Any
    do { obj = try JSONSerialization.jsonObject(with: data) }
    catch { throw MapError(msg: "The mapping file is not valid JSON (\(error.localizedDescription))") }
    guard let root = obj as? [String: Any] else { throw MapError(msg: "The mapping file must be a JSON object") }
    let n = (root["numLeds"] as? NSNumber)?.intValue ?? 150
    guard let arr = root["segments"] as? [[String: Any]] else { throw MapError(msg: "\"segments\" list is missing") }
    var segs: [Segment] = []
    for (k, d) in arr.enumerated() {
        let tag = "segment \(k + 1)"
        guard let first = (d["first"] as? NSNumber)?.intValue, let last = (d["last"] as? NSNumber)?.intValue else {
            throw MapError(msg: "\(tag): \"first\" and \"last\" are needed")
        }
        guard let sideName = d["side"] as? String, let si = sideNames.firstIndex(of: sideName.lowercased()) else {
            throw MapError(msg: "\(tag): \"side\" must be top, right, bottom or left")
        }
        let dir = ((d["direction"] as? String) ?? "forward").lowercased()
        guard dir == "forward" || dir == "reverse" else { throw MapError(msg: "\(tag): \"direction\" must be forward or reverse") }
        let a = (d["from"] as? NSNumber)?.doubleValue ?? 0.0
        let b = (d["to"] as? NSNumber)?.doubleValue ?? 1.0
        segs.append(Segment(first: first, last: last, side: si, reverse: dir == "reverse", from: a, to: b))
    }
    return Mapping(numLeds: n, segs: segs)
}

func validate(_ m: Mapping) -> [String] {
    var problems: [String] = []
    var owner: [Int: Int] = [:]
    if m.numLeds < 1 || m.numLeds > 255 { problems.append("numLeds must be between 1 and 255") }
    for (k, s) in m.segs.enumerated() {
        let tag = "segment \(k + 1) (LEDs \(s.first)-\(s.last), \(sideNames[s.side]))"
        if s.last < s.first { problems.append("\(tag): last is smaller than first") }
        if s.first < 0 || s.last >= m.numLeds { problems.append("\(tag): LED numbers must be 0...\(m.numLeds - 1)") }
        if !(0.0 <= s.from && s.from < s.to && s.to <= 1.0) { problems.append("\(tag): need 0 <= from < to <= 1") }
        if s.first <= s.last {
            for n in max(s.first, 0)...max(min(s.last, m.numLeds - 1), max(s.first, 0)) {
                if let o = owner[n], o != k { problems.append("\(tag): LED \(n) is already used by segment \(o + 1)"); break }
                owner[n] = k
            }
        }
    }
    return problems
}

// =====================================================================================
//  COLOR MATH (a line-by-line port of ambilight.py so the result is identical)
// =====================================================================================

/// Average a slice of a profile (flat r,g,b,... doubles) into n equal bins -> flat n*3 doubles.
func binMeans(_ prof: [Double], lo: Int, hi: Int, n: Int) -> [Double] {
    let L = hi - lo
    var cs = [Double](repeating: 0, count: (L + 1) * 3)
    for i in 0..<L {
        for c in 0..<3 { cs[(i + 1) * 3 + c] = cs[i * 3 + c] + prof[(lo + i) * 3 + c] }
    }
    // np.linspace(0, L, n + 1).round()  (round half to even), last point exactly L
    let step = Double(L) / Double(n)
    var e0 = [Int](repeating: 0, count: n + 1)
    for k in 0...n {
        let v = (k == n) ? Double(L) : Double(k) * step
        e0[k] = Int(v.rounded(.toNearestOrEven))
    }
    var e = e0
    if n >= 1 { for k in 1...n { e[k] = max(e0[k], e0[k - 1] + 1) } }    // uses the ORIGINAL previous edge, like numpy
    for k in 0...n { e[k] = min(e[k], L) }
    var out = [Double](repeating: 0, count: n * 3)
    for j in 0..<n {
        let width = Double(max(e[j + 1] - e[j], 1))
        for c in 0..<3 { out[j * 3 + c] = (cs[e[j + 1] * 3 + c] - cs[e[j] * 3 + c]) / width }
    }
    return out
}

/// profiles: 4 flat arrays (top, right, bottom, left), natural direction, RGB. Returns flat numLeds*3 floats.
func frameFromProfiles(_ profiles: [[Double]], _ m: Mapping) -> [Float] {
    var frame = [Float](repeating: 0, count: m.numLeds * 3)
    for s in m.segs {
        let prof = profiles[s.side]
        let L = prof.count / 3
        if L < 1 { continue }
        var lo = Int((s.from * Double(L)).rounded(.toNearestOrEven))
        var hi = max(lo + 1, Int((s.to * Double(L)).rounded(.toNearestOrEven)))
        hi = min(hi, L)
        lo = min(lo, hi - 1)
        let cnt = s.last - s.first + 1
        if cnt < 1 { continue }
        let cols = binMeans(prof, lo: lo, hi: hi, n: cnt)
        for j in 0..<cnt {
            let src = s.reverse ? (cnt - 1 - j) : j
            let dst = s.first + j
            if dst < 0 || dst >= m.numLeds { continue }
            for c in 0..<3 { frame[dst * 3 + c] = Float(cols[src * 3 + c]) }
        }
    }
    return frame
}

struct Look {
    var saturation: Float = 1.25
    var gain: Float = 1.0
    var wb: (Float, Float, Float) = (1.0, 0.92, 0.78)
}

func toByte(_ v: Float) -> UInt8 {
    if !(v > 0) { return 0 }
    if v >= 255 { return 255 }
    return UInt8(v)                       // truncates, like numpy astype(uint8)
}

func tweak(_ frame: [Float], _ look: Look) -> [UInt8] {
    let n = frame.count / 3
    var out = [UInt8](repeating: 0, count: n * 3)
    for i in 0..<n {
        var r = frame[i * 3], g = frame[i * 3 + 1], b = frame[i * 3 + 2]
        if look.saturation != 1.0 {
            let gray = (r + g + b) / 3
            r = gray + (r - gray) * look.saturation
            g = gray + (g - gray) * look.saturation
            b = gray + (b - gray) * look.saturation
        }
        r = min(max(r * look.gain * look.wb.0, 0), 255)
        g = min(max(g * look.gain * look.wb.1, 0), 255)
        b = min(max(b * look.gain * look.wb.2, 0), 255)
        if max(r, max(g, b)) < blackCutoff { continue }
        out[i * 3] = toByte(r); out[i * 3 + 1] = toByte(g); out[i * 3 + 2] = toByte(b)
    }
    return out
}

/// Top red, right green, bottom blue, left yellow; each side brightens in its forward direction; first LED of every segment white.
func testFrame(_ m: Mapping) -> [UInt8] {
    let colors: [(Double, Double, Double)] = [(255, 0, 0), (0, 255, 0), (0, 80, 255), (255, 200, 0)]
    let L = 400
    let step = 0.8 / Double(L - 1)
    var profiles: [[Double]] = []
    for c in colors {
        var p = [Double](repeating: 0, count: L * 3)
        for i in 0..<L {
            let ramp = (i == L - 1) ? 1.0 : 0.2 + Double(i) * step
            p[i * 3] = c.0 * ramp; p[i * 3 + 1] = c.1 * ramp; p[i * 3 + 2] = c.2 * ramp
        }
        profiles.append(p)
    }
    var frame = frameFromProfiles(profiles, m)
    for s in m.segs where s.first >= 0 && s.first < m.numLeds {
        frame[s.first * 3] = 255; frame[s.first * 3 + 1] = 255; frame[s.first * 3 + 2] = 255
    }
    return frame.map { toByte($0) }
}

/// USB frame: A5 5A, count, R,G,B x count, XOR checksum of count and all color bytes.
func serialPacket(_ rgb: [UInt8]) -> [UInt8] {
    let count = UInt8(rgb.count / 3)
    var check = count
    for b in rgb { check ^= b }
    return [0xA5, 0x5A, count] + rgb + [check]
}

let releasePacket: [UInt8] = [0xA5, 0x5A, 0, 0]       // "app stopped": the strip returns to its playlist

// =====================================================================================
//  SCREEN EDGES -> PROFILES
// =====================================================================================
func edgeProfiles(_ pb: CVPixelBuffer) -> [[Double]] {
    CVPixelBufferLockBaseAddress(pb, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
    let bpr = CVPixelBufferGetBytesPerRow(pb)
    guard w > 4, h > 4, let base = CVPixelBufferGetBaseAddress(pb) else { return [] }
    let p = base.assumingMemoryBound(to: UInt8.self)
    let dh = max(1, Int(Double(h) * edgeDepth)), dw = max(1, Int(Double(w) * edgeDepth))

    var top = [Double](repeating: 0, count: w * 3)
    var bottom = [Double](repeating: 0, count: w * 3)
    var left = [Double](repeating: 0, count: h * 3)
    var right = [Double](repeating: 0, count: h * 3)

    for y in 0..<dh {
        let rowT = p + y * bpr
        let rowB = p + (h - dh + y) * bpr
        for x in 0..<w {
            top[x * 3]     += Double(rowT[x * 4 + 2]); top[x * 3 + 1]     += Double(rowT[x * 4 + 1]); top[x * 3 + 2]     += Double(rowT[x * 4])
            bottom[x * 3]  += Double(rowB[x * 4 + 2]); bottom[x * 3 + 1]  += Double(rowB[x * 4 + 1]); bottom[x * 3 + 2]  += Double(rowB[x * 4])
        }
    }
    for y in 0..<h {
        let row = p + y * bpr
        for x in 0..<dw {
            left[y * 3]  += Double(row[x * 4 + 2]); left[y * 3 + 1]  += Double(row[x * 4 + 1]); left[y * 3 + 2]  += Double(row[x * 4])
            let xr = w - dw + x
            right[y * 3] += Double(row[xr * 4 + 2]); right[y * 3 + 1] += Double(row[xr * 4 + 1]); right[y * 3 + 2] += Double(row[xr * 4])
        }
    }
    let dT = Double(dh), dL = Double(dw)
    for i in 0..<top.count { top[i] /= dT; bottom[i] /= dT }
    for i in 0..<left.count { left[i] /= dL; right[i] /= dL }
    return [top, right, bottom, left]
}

// =====================================================================================
//  USB SERIAL
// =====================================================================================
final class SerialLink {
    private var fd: Int32 = -1
    private(set) var path = ""
    var isOpen: Bool { return fd >= 0 }

    static func ports() -> [String] {
        let prefixes = ["cu.usbserial", "cu.SLAB_USBtoUART", "cu.wchusbserial", "cu.usbmodem"]
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        return names.filter { n in prefixes.contains { n.hasPrefix($0) } }.sorted().map { "/dev/" + $0 }
    }

    func open(preferred: String?) -> Bool {
        close()
        let list = SerialLink.ports()
        let target: String
        if let p = preferred, list.contains(p) { target = p }
        else if let f = list.first { target = f }
        else { return false }

        let f = Darwin.open(target, O_RDWR | O_NOCTTY | O_NONBLOCK)
        if f < 0 { return false }
        var t = termios()
        if tcgetattr(f, &t) != 0 { Darwin.close(f); return false }
        cfmakeraw(&t)
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        cfsetspeed(&t, speed_t(serialBaud))
        if tcsetattr(f, TCSANOW, &t) != 0 { Darwin.close(f); return false }

        // keep DTR and RTS low so opening the port does not reset the ESP32
        var bits: Int32 = 0x002 | 0x004                 // TIOCM_DTR | TIOCM_RTS
        _ = ioctl(f, 0x8004746B, &bits)                 // TIOCMBIC (clear modem bits)

        fd = f
        path = target
        usleep(400_000)                                 // in case the board restarted when the port opened
        tcflush(f, TCIOFLUSH)
        return true
    }

    /// false = the link is broken (cable pulled), true = ok or just skipped a frame because the board was busy
    func write(_ data: [UInt8]) -> Bool {
        if fd < 0 { return false }
        var off = 0
        let deadline = DispatchTime.now().uptimeNanoseconds + 50_000_000
        while off < data.count {
            let n = data.withUnsafeBytes { raw -> Int in
                return Darwin.write(fd, raw.baseAddress! + off, data.count - off)
            }
            if n > 0 { off += n }
            else if n < 0 && (errno == EAGAIN || errno == EINTR) {
                if DispatchTime.now().uptimeNanoseconds > deadline { return true }
                usleep(300)
            } else { return false }
        }
        return true
    }

    func flushInput() { if fd >= 0 { tcflush(fd, TCIFLUSH) } }

    func close() {
        if fd >= 0 { Darwin.close(fd) }
        fd = -1
    }
}

// =====================================================================================
//  ENGINE: capture -> map -> send   (everything below runs on one serial queue)
// =====================================================================================
enum SyncMode { case sync, test, white }

final class Engine: NSObject, SCStreamOutput, SCStreamDelegate {
    let q = DispatchQueue(label: "ledsync.engine", qos: .userInteractive)
    var onStatus: ((String, Bool) -> Void)?                // always called on the main thread

    // state owned by q
    private var mapping: Mapping
    private var look = Look()
    private var displayID: CGDirectDisplayID = 0           // 0 = main display
    private var portPref: String?
    private var running = false
    private var mode: SyncMode = .sync
    private var gen = 0
    private var link = SerialLink()
    private var stream: SCStream?
    private var timer: DispatchSourceTimer?
    private var lastPacket: [UInt8]?
    private var staticPacket: [UInt8]?
    private var lastSend: TimeInterval = 0
    private var lastOpenTry: TimeInterval = 0
    private var lastReport: TimeInterval = 0
    private var frames = 0
    private var fps = 0
    private var sendCount = 0
    private var lastNote = ""

    init(mapping: Mapping) { self.mapping = mapping }

    // ---- settings (call from any thread) ----
    func setLook(_ l: Look) { q.async { self.look = l } }
    func setMapping(_ m: Mapping) { q.async { self.mapping = m } }
    func setPort(_ p: String?) { q.async { self.portPref = p; if self.running { self.link.close() } } }
    func setDisplay(_ d: CGDirectDisplayID) {
        q.async {
            self.displayID = d
            if self.running && self.mode == .sync { self.startLocked(.sync) }
        }
    }

    /// Ask the ESP32 to play one of its built-in patterns (stops screen sync first).
    /// cmd 1 = effect number, cmd 2 = playlist (1 Mix, 2 Chill, 3 Crazy).
    func sendPattern(cmd: UInt8, arg: UInt8, label: String) {
        q.async {
            self.stopLocked(release: false)
            self.post("Sending “\(label)”…", active: false)
            let l = SerialLink()
            guard l.open(preferred: self.portPref) else {
                self.post("Can't find the ESP32 on USB", active: false)
                return
            }
            let pkt: [UInt8] = [0xA5, 0x5A, 0xFE, cmd, arg, 0xFE ^ cmd ^ arg]
            // Repeated for ~2 s in case the board was restarting when the port opened. The firmware ignores repeats.
            for _ in 0..<12 {
                if !l.write(pkt) { break }
                usleep(150_000)
            }
            l.close()
            self.post("Playing: \(label)", active: false)
        }
    }

    func start(_ m: SyncMode) { q.async { self.startLocked(m) } }
    func stop() { q.async { self.stopLocked(release: true) } }
    func stopAndWait() { q.sync { self.stopLocked(release: true) } }
    func isRunning() -> Bool { return q.sync { running } }

    private func post(_ text: String, active: Bool) {
        lastNote = text
        DispatchQueue.main.async { self.onStatus?(text, active) }
    }

    // ---- start / stop ----
    private func startLocked(_ m: SyncMode) {
        stopLocked(release: false)
        let problems = validate(mapping)
        if !problems.isEmpty { post("Mapping problem: " + problems[0], active: false); return }
        running = true
        mode = m
        gen += 1
        let g = gen
        frames = 0; fps = 0; sendCount = 0
        lastPacket = nil
        lastReport = Date().timeIntervalSince1970
        switch m {
        case .test:  staticPacket = serialPacket(testFrame(mapping))
        case .white:
            let f = tweak([Float](repeating: 200, count: mapping.numLeds * 3), look)
            staticPacket = serialPacket(f)
        case .sync:  staticPacket = nil
        }
        if link.open(preferred: portPref) {
            post(m == .sync ? "Starting screen capture…" : (m == .test ? "Test pattern" : "White check"), active: true)
        } else {
            post("Waiting for the ESP32 on USB…", active: true)
        }
        lastOpenTry = Date().timeIntervalSince1970

        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + 0.05, repeating: 0.05)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t

        if m == .sync { startCapture(g) }
    }

    private func stopLocked(release: Bool) {
        let wasRunning = running
        running = false
        gen += 1
        timer?.cancel(); timer = nil
        if let s = stream {
            stream = nil
            Task { try? await s.stopCapture() }
        }
        if wasRunning && release && link.isOpen {
            for _ in 0..<3 { _ = link.write(releasePacket); usleep(20_000) }
        }
        link.close()
        lastPacket = nil
        if wasRunning { post("Stopped", active: false) }
    }

    // ---- screen capture ----
    private func startCapture(_ g: Int) {
        let wanted = displayID
        Task { [weak self] in
            guard let self = self else { return }
            do {
                let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
                let id = wanted != 0 ? wanted : CGMainDisplayID()
                guard let display = content.displays.first(where: { $0.displayID == id }) ?? content.displays.first else {
                    self.q.async { if self.gen == g { self.post("No display found", active: false) } }
                    return
                }
                let cfg = SCStreamConfiguration()
                cfg.width = captureWidth
                cfg.height = max(16, Int((Double(captureWidth) * Double(display.height) / Double(max(display.width, 1))).rounded()))
                cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(targetFPS))
                cfg.pixelFormat = kCVPixelFormatType_32BGRA
                cfg.queueDepth = 3
                cfg.showsCursor = false
                let filter = SCContentFilter(display: display, excludingWindows: [])
                let s = SCStream(filter: filter, configuration: cfg, delegate: self)
                try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.q)
                try await s.startCapture()
                self.q.async {
                    if self.gen == g && self.running { self.stream = s }
                    else { Task { try? await s.stopCapture() } }
                }
            } catch {
                self.q.async {
                    if self.gen == g {
                        self.stopLocked(release: false)
                        self.post("Screen capture failed. Allow “LED Sync” under Privacy & Security > Screen & System Audio Recording, then reopen the app.", active: false)
                    }
                }
            }
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        q.async {
            guard self.running, self.mode == .sync else { return }
            let g = self.gen
            self.post("Capture interrupted, retrying…", active: true)
            self.q.asyncAfter(deadline: .now() + 2) {
                if self.gen == g && self.running { self.startLocked(.sync) }
            }
        }
    }

    // runs on q (sampleHandlerQueue)
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sb.isValid, running, mode == .sync else { return }
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
           let raw = arr.first?[.status] as? Int, let st = SCFrameStatus(rawValue: raw), st != .complete { return }
        guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let profiles = edgeProfiles(pb)
        if profiles.count != 4 { return }
        let bytes = tweak(frameFromProfiles(profiles, mapping), look)
        let pkt = serialPacket(bytes)
        lastPacket = pkt
        frames += 1
        sendRaw(pkt)
    }

    // ---- sending ----
    private func sendRaw(_ pkt: [UInt8]) {
        guard link.isOpen else { return }
        lastSend = Date().timeIntervalSince1970
        if !link.write(pkt) {
            link.close()
            post("USB link lost. Reconnecting…", active: true)
            return
        }
        sendCount += 1
        if sendCount % 60 == 0 { link.flushInput() }       // throw away anything the board printed
    }

    private func tick() {
        guard running else { return }
        let now = Date().timeIntervalSince1970
        if !link.isOpen {
            if now - lastOpenTry > 1.0 {
                lastOpenTry = now
                if link.open(preferred: portPref) {
                    post(mode == .sync ? "Connected: \(link.path)" : lastNote, active: true)
                }
            }
            return
        }
        switch mode {
        case .test, .white:
            if let p = staticPacket { sendRaw(p) }
        case .sync:
            // a still screen produces no new frames; keep the strip alive
            if let p = lastPacket, now - lastSend > 0.25 { sendRaw(p) }
        }
        if now - lastReport >= 2.0 {
            fps = Int((Double(frames) / (now - lastReport)).rounded())
            frames = 0; lastReport = now
            if mode == .sync {
                let short = (link.path as NSString).lastPathComponent
                post("Syncing · \(fps) fps · \(short)", active: true)
            }
        }
    }
}

// =====================================================================================
//  APP (menu bar)
// =====================================================================================
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let defaults = UserDefaults.standard
    var engine: Engine!
    var mapping: Mapping
    var mappingError: String?
    var statusText = "Stopped"
    var isActive = false
    var currentPattern: String?
    weak var statusLine: NSMenuItem?

    // Patterns built into the ESP32 sketch: (name, command, number). command 1 = effect, 2 = playlist.
    let extraPlaylists: [(String, UInt8, UInt8)] = [("Mix (auto)", 2, 1), ("Chill (auto)", 2, 2), ("Crazy (auto)", 2, 3)]
    let extraRelaxing: [(String, UInt8, UInt8)] = [
        ("Breathing", 1, 7), ("Sunset", 1, 8), ("Raindrops", 1, 9), ("Candlelight", 1, 10), ("Fireflies", 1, 11),
        ("Starry night", 1, 12), ("Pastel drift", 1, 13), ("Aurora", 1, 0), ("Lava lamp", 1, 1), ("Ocean", 1, 4),
    ]
    let extraCrazy: [(String, UInt8, UInt8)] = [
        ("Fire", 1, 2), ("Hyperspace", 1, 3), ("Lightning", 1, 5), ("Kaleidoscope", 1, 6),
        ("Tron Legacy (blue)", 1, 17), ("Tron: Clu (orange)", 1, 18), ("Tron: Ares (red)", 1, 19), ("Wi-Fi radar", 1, 14),
    ]

    let gainOptions: [(String, Float)] = [("50%", 0.5), ("75%", 0.75), ("100%", 1.0), ("125%", 1.25), ("150%", 1.5)]
    let satOptions: [(String, Float)] = [("Natural (1.0)", 1.0), ("Vivid (1.25)", 1.25), ("Very vivid (1.5)", 1.5), ("Extreme (1.8)", 1.8)]
    let wbOptions: [(String, (Float, Float, Float))] = [
        ("Off (1.0, 1.0, 1.0)", (1.0, 1.0, 1.0)),
        ("Slightly warm", (1.0, 0.96, 0.88)),
        ("Warm - default", (1.0, 0.92, 0.78)),
        ("Warmer", (1.0, 0.88, 0.68)),
    ]

    var mappingURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("LEDSync", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("mapping.json")
    }

    override init() {
        mapping = (try? parseMapping(Data(defaultMappingJSON.utf8))) ?? Mapping(numLeds: 150, segs: [])
        super.init()
    }

    // ---- preferences ----
    var gain: Float { get { defaults.object(forKey: "gain") as? Float ?? 1.0 } set { defaults.set(newValue, forKey: "gain") } }
    var sat: Float { get { defaults.object(forKey: "sat") as? Float ?? 1.25 } set { defaults.set(newValue, forKey: "sat") } }
    var wbIndex: Int { get { defaults.object(forKey: "wb") as? Int ?? 2 } set { defaults.set(newValue, forKey: "wb") } }
    var displayPref: UInt32 { get { UInt32(defaults.integer(forKey: "display")) } set { defaults.set(Int(newValue), forKey: "display") } }
    var portPref: String? { get { defaults.string(forKey: "port") } set { defaults.set(newValue, forKey: "port") } }
    var autoStart: Bool { get { defaults.object(forKey: "autostart") as? Bool ?? true } set { defaults.set(newValue, forKey: "autostart") } }

    func currentLook() -> Look {
        let i = min(max(wbIndex, 0), wbOptions.count - 1)
        return Look(saturation: sat, gain: gain, wb: wbOptions[i].1)
    }

    // ---- launch ----
    func applicationDidFinishLaunching(_ notification: Notification) {
        loadMapping()
        engine = Engine(mapping: mapping)
        engine.setLook(currentLook())
        engine.setDisplay(displayPref)
        engine.setPort(portPref)
        engine.onStatus = { [weak self] text, active in
            guard let self = self else { return }
            self.statusText = text
            self.isActive = active
            if active { self.currentPattern = nil }
            self.statusLine?.title = text
            self.updateIcon()
        }

        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
        updateIcon()

        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self = self, self.isActive else { return }
            self.engine.start(.sync)
        }

        if autoStart && mappingError == nil && CGPreflightScreenCaptureAccess() {
            engine.start(.sync)
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        engine.stopAndWait()               // sends the "release" packet so the strip returns to its playlist
    }

    func loadMapping() {
        let url = mappingURL
        if !FileManager.default.fileExists(atPath: url.path) {
            try? defaultMappingJSON.write(to: url, atomically: true, encoding: .utf8)
        }
        do {
            let data = try Data(contentsOf: url)
            let m = try parseMapping(data)
            let problems = validate(m)
            if let p = problems.first { mappingError = p } else { mappingError = nil; mapping = m }
        } catch let e as MapError {
            mappingError = e.msg
        } catch {
            mappingError = "Could not read mapping.json"
        }
    }

    func updateIcon() {
        guard let b = statusItem.button else { return }
        let name = isActive ? "lightbulb.fill" : "lightbulb"
        if let img = NSImage(systemSymbolName: name, accessibilityDescription: "LED Sync") {
            img.isTemplate = true
            b.image = img
            b.title = ""
        } else {
            b.image = nil
            b.title = isActive ? "LED●" : "LED"
        }
    }

    // ---- menu ----
    func menuNeedsUpdate(_ menu: NSMenu) { rebuild(menu) }

    func item(_ title: String, _ sel: Selector?, checked: Bool = false, rep: Any? = nil, enabled: Bool = true) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        i.target = self
        i.state = checked ? .on : .off
        i.representedObject = rep
        i.isEnabled = enabled && sel != nil
        return i
    }

    func rebuild(_ m: NSMenu) {
        m.removeAllItems()

        let st = item(mappingError.map { "Mapping problem: \($0)" } ?? statusText, nil)
        statusLine = st
        m.addItem(st)
        m.addItem(.separator())

        m.addItem(item(isActive ? "Stop syncing" : "Start syncing", #selector(toggleSync)))
        m.addItem(.separator())

        // Display
        let dispMenu = NSMenu()
        dispMenu.addItem(item("Main display", #selector(pickDisplay(_:)), checked: displayPref == 0, rep: NSNumber(value: 0)))
        for s in NSScreen.screens {
            if let num = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber {
                dispMenu.addItem(item(s.localizedName, #selector(pickDisplay(_:)), checked: displayPref == num.uint32Value, rep: num))
            }
        }
        let dispItem = NSMenuItem(title: "Display", action: nil, keyEquivalent: ""); dispItem.submenu = dispMenu
        m.addItem(dispItem)

        // USB port
        let portMenu = NSMenu()
        portMenu.addItem(item("Automatic", #selector(pickPort(_:)), checked: portPref == nil, rep: ""))
        for p in SerialLink.ports() {
            portMenu.addItem(item((p as NSString).lastPathComponent, #selector(pickPort(_:)), checked: portPref == p, rep: p))
        }
        let portItem = NSMenuItem(title: "USB port", action: nil, keyEquivalent: ""); portItem.submenu = portMenu
        m.addItem(portItem)

        // Look
        let lookMenu = NSMenu()
        lookMenu.addItem(item("Brightness", nil, enabled: false))
        for (i, o) in gainOptions.enumerated() {
            lookMenu.addItem(item("  " + o.0, #selector(pickGain(_:)), checked: abs(gain - o.1) < 0.001, rep: NSNumber(value: i)))
        }
        lookMenu.addItem(.separator())
        lookMenu.addItem(item("Color intensity", nil, enabled: false))
        for (i, o) in satOptions.enumerated() {
            lookMenu.addItem(item("  " + o.0, #selector(pickSat(_:)), checked: abs(sat - o.1) < 0.001, rep: NSNumber(value: i)))
        }
        lookMenu.addItem(.separator())
        lookMenu.addItem(item("White balance", nil, enabled: false))
        for (i, o) in wbOptions.enumerated() {
            lookMenu.addItem(item("  " + o.0, #selector(pickWB(_:)), checked: wbIndex == i, rep: NSNumber(value: i)))
        }
        let lookItem = NSMenuItem(title: "Look", action: nil, keyEquivalent: ""); lookItem.submenu = lookMenu
        m.addItem(lookItem)

        // Tools
        let tools = NSMenu()
        tools.addItem(item("Test pattern (check the mapping)", #selector(runTest)))
        tools.addItem(item("White check (tune white balance)", #selector(runWhite)))
        tools.addItem(.separator())
        tools.addItem(item("Edit mapping file…", #selector(editMapping)))
        tools.addItem(item("Reload mapping file", #selector(reloadMapping)))
        let toolItem = NSMenuItem(title: "Tools", action: nil, keyEquivalent: ""); toolItem.submenu = tools
        m.addItem(toolItem)

        // Extras: the patterns built into the ESP32 (needs the updated sketch)
        let extras = NSMenu()
        func addGroup(_ title: String, _ list: [(String, UInt8, UInt8)]) {
            extras.addItem(item(title, nil, enabled: false))
            for (name, cmd, arg) in list {
                extras.addItem(item("  " + name, #selector(pickPattern(_:)),
                                    checked: !isActive && currentPattern == name,
                                    rep: NSNumber(value: Int(cmd) * 256 + Int(arg))))
            }
        }
        addGroup("Playlists", extraPlaylists)
        extras.addItem(.separator())
        addGroup("Relaxing", extraRelaxing)
        extras.addItem(.separator())
        addGroup("Crazy", extraCrazy)
        let extraItem = NSMenuItem(title: "Extras", action: nil, keyEquivalent: ""); extraItem.submenu = extras
        m.addItem(extraItem)

        m.addItem(.separator())
        m.addItem(item("Start syncing when the app opens", #selector(toggleAutoStart), checked: autoStart))
        if #available(macOS 13.0, *) {
            m.addItem(item("Open at login", #selector(toggleLogin), checked: SMAppService.mainApp.status == .enabled))
        }
        m.addItem(.separator())
        m.addItem(item("Quit LED Sync", #selector(quit)))
    }

    // ---- actions ----
    @objc func toggleSync() {
        if isActive { engine.stop(); return }
        startSync()
    }

    func startSync() {
        if let e = mappingError { statusText = "Mapping problem: \(e)"; return }
        if !CGPreflightScreenCaptureAccess() {
            _ = CGRequestScreenCaptureAccess()
            let a = NSAlert()
            a.messageText = "LED Sync needs Screen Recording permission"
            a.informativeText = "Switch on “LED Sync” in System Settings > Privacy & Security > Screen & System Audio Recording. Only this app gets the permission, not Terminal.\n\nAfter switching it on, quit LED Sync from its menu and open it again."
            a.addButton(withTitle: "Open System Settings")
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            if a.runModal() == .alertFirstButtonReturn,
               let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
                NSWorkspace.shared.open(url)
            }
            return
        }
        engine.start(.sync)
    }

    @objc func pickDisplay(_ s: NSMenuItem) {
        displayPref = (s.representedObject as? NSNumber)?.uint32Value ?? 0
        engine.setDisplay(displayPref)
    }
    @objc func pickPort(_ s: NSMenuItem) {
        let p = (s.representedObject as? String) ?? ""
        portPref = p.isEmpty ? nil : p
        engine.setPort(portPref)
    }
    @objc func pickGain(_ s: NSMenuItem) {
        if let i = (s.representedObject as? NSNumber)?.intValue { gain = gainOptions[i].1; engine.setLook(currentLook()) }
    }
    @objc func pickSat(_ s: NSMenuItem) {
        if let i = (s.representedObject as? NSNumber)?.intValue { sat = satOptions[i].1; engine.setLook(currentLook()) }
    }
    @objc func pickWB(_ s: NSMenuItem) {
        if let i = (s.representedObject as? NSNumber)?.intValue {
            wbIndex = i
            engine.setLook(currentLook())
            if statusText == "White check" { engine.start(.white) }
        }
    }
    @objc func pickPattern(_ s: NSMenuItem) {
        guard let v = (s.representedObject as? NSNumber)?.intValue else { return }
        let label = s.title.trimmingCharacters(in: .whitespaces)
        currentPattern = label
        engine.sendPattern(cmd: UInt8(v >> 8), arg: UInt8(v & 255), label: label)
    }
    @objc func runTest() {
        if let e = mappingError { statusText = "Mapping problem: \(e)"; return }
        engine.start(.test)
    }
    @objc func runWhite() {
        if let e = mappingError { statusText = "Mapping problem: \(e)"; return }
        engine.start(.white)
    }
    @objc func editMapping() {
        _ = mappingURL
        loadMapping()      // makes sure the file exists
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        p.arguments = ["-t", mappingURL.path]
        try? p.run()
    }
    @objc func reloadMapping() {
        loadMapping()
        if mappingError == nil {
            engine.setMapping(mapping)
            statusText = "Mapping reloaded"
        } else {
            statusText = "Mapping problem: \(mappingError!)"
        }
        statusLine?.title = statusText
    }
    @objc func toggleAutoStart() { autoStart.toggle() }
    @objc func toggleLogin() {
        if #available(macOS 13.0, *) {
            do {
                if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() }
                else { try SMAppService.mainApp.register() }
            } catch {
                let a = NSAlert()
                a.messageText = "Could not change the login setting"
                a.informativeText = "Move LED Sync.app into the Applications folder and try again.\n\(error.localizedDescription)"
                NSApp.activate(ignoringOtherApps: true)
                a.runModal()
            }
        }
    }
    @objc func quit() { NSApp.terminate(nil) }
}

// =====================================================================================
//  SELF TEST: proves the Swift math gives the same bytes as ambilight.py
// =====================================================================================
func fnv1a(_ bytes: [UInt8]) -> UInt32 {
    var h: UInt32 = 2166136261
    for b in bytes { h = (h ^ UInt32(b)) &* 16777619 }
    return h
}

func runSelfTest() -> Int32 {
    guard let m = try? parseMapping(Data(defaultMappingJSON.utf8)) else { print("FAIL: default mapping did not parse"); return 1 }
    var ok = true
    let problems = validate(m)
    if !problems.isEmpty { print("FAIL: default mapping invalid: \(problems)"); ok = false }

    var profiles: [[Double]] = []
    for s in 0..<4 {
        var p = [Double](repeating: 0, count: 400 * 3)
        for i in 0..<400 { for c in 0..<3 { p[i * 3 + c] = Double((i * 7 + s * 31 + c * 53) % 256) } }
        profiles.append(p)
    }
    let pkt = serialPacket(tweak(frameFromProfiles(profiles, m), Look()))
    let h1 = fnv1a(pkt)
    let e1: UInt32 = 0xd4aa6978
    print("frame packet : \(pkt.count) bytes, hash \(String(h1, radix: 16))  expected \(String(e1, radix: 16))  \(h1 == e1 ? "OK" : "MISMATCH")")
    if h1 != e1 || pkt.count != 454 { ok = false }

    let h2 = fnv1a(serialPacket(testFrame(m)))
    let e2: UInt32 = 0x2a73bcd2
    print("test pattern : hash \(String(h2, radix: 16))  expected \(String(e2, radix: 16))  \(h2 == e2 ? "OK" : "MISMATCH")")
    if h2 != e2 { ok = false }

    let w = tweak([Float](repeating: 200, count: 150 * 3), Look())
    print("white check  : \(w[0]),\(w[1]),\(w[2])  expected 200,184,156  \((w[0], w[1], w[2]) == (200, 184, 156) ? "OK" : "MISMATCH")")
    if (w[0], w[1], w[2]) != (200, 184, 156) { ok = false }

    print(ok ? "SELFTEST PASSED" : "SELFTEST FAILED")
    return ok ? 0 : 1
}

// =====================================================================================
//  ENTRY POINT
// =====================================================================================
if CommandLine.arguments.contains("--selftest") {
    exit(runSelfTest())
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)      // menu bar only, no Dock icon
app.run()

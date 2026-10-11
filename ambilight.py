#!/usr/bin/env python3
"""
Screen -> LED strip (ambilight) for Mac + the ESP32 "Screen sync" effect.

Commands:
  python3 ambilight.py info          print how every LED is mapped to the screen
  python3 ambilight.py test          side-color test pattern (check your mapping with this)
  python3 ambilight.py led 37        light only LED number 37
  python3 ambilight.py range 40 70   light LEDs 40 to 70 (to check a segment)
  python3 ambilight.py scan          light LEDs one at a time, in order  (scan 0.15 = faster)
  python3 ambilight.py run           the real thing: strip follows the screen  (default)
  python3 ambilight.py bench         measure how long capture takes (for fixing latency)
  python3 ambilight.py white         show a corrected white on the whole strip (to tune WHITE_BALANCE)

Setup (once):
  python3 -m venv ~/ambi && source ~/ambi/bin/activate && pip install mss numpy pyserial
  Give Terminal screen recording permission, then restart Terminal.
"""
import sys, time, socket, base64, threading, urllib.request
from concurrent.futures import ThreadPoolExecutor
import numpy as np

# ===================== SETTINGS =====================
TRANSPORT  = "usb"              # "usb" = colors go over the USB cable (steady, low delay)   "wifi" = over Wi-Fi
SERIAL_PORT = None              # USB: e.g. "/dev/cu.usbserial-0001"; None = find it automatically
SERIAL_BAUD = 921600            # USB speed, must match SYNC_BAUD in the sketch

ESP_HOST   = "ledstrip.local"   # Wi-Fi mode only: or put the ESP32's IP address here, e.g. "192.168.1.45"
WEB_PASS   = "changeme"         # Wi-Fi mode only: the page password in the sketch (user name is always admin); "" = none
UDP_PORT   = 4210               # Wi-Fi mode only: must match SYNC_PORT in the sketch
NUM_LEDS   = 150                # must match NUM_LEDS in the sketch

MONITOR    = 2                  # 1 = main display, 2 = second display ...
FPS        = 60                 # target frames per second (the real rate is limited by how fast the Mac can capture)
EDGE_DEPTH = 0.06               # how deep into the screen to sample (0.06 = outer 6 percent; smaller = faster)
SAMPLE_STEP = 4                 # use every Nth pixel when averaging (higher = faster, 4 is plenty)
PARALLEL_GRAB = True            # capture the 4 screen edges at the same time (run 'bench' to see if it helps)

# ---------------------------------------------------------------------------
# MANUAL MAPPING  (this is the part you edit)
#
# Each line says: "LEDs FIRST to LAST show SIDE of the screen, running DIRECTION".
#   (first, last, side, direction)                      the whole side
#   (first, last, side, direction, from_part, to_part)  only part of the side
#
#   side       "top", "right", "bottom" or "left"   (as seen from the FRONT of the screen)
#   direction  "forward": top/bottom run LEFT -> RIGHT, left/right run TOP -> BOTTOM
#              "reverse": the opposite
#   from_part, to_part   0.0 to 1.0 along that side in the FORWARD direction.
#              0.0 = left end (top/bottom) or top end (left/right); 1.0 = the other end.
#              Example: the right half of the bottom edge is 0.5 to 1.0.
#
# Both laps go in this list, one line per piece. LEDs not mentioned stay dark.
# Leave SEGMENTS = None to use the automatic corner-to-corner layout below instead.
#
# Example: strip starts at the MIDDLE of the bottom edge and runs around clockwise (seen from
# the front), twice, 75 LEDs per lap:
#
# SEGMENTS = [
#     # lap 1
#     (0,   11, "bottom", "reverse", 0.0, 0.5),    # bottom centre -> bottom-left corner
#     (12,  24, "left",   "reverse"),              # up the left side
#     (25,  48, "top",    "forward"),              # across the top, left to right
#     (49,  62, "right",  "forward"),              # down the right side
#     (63,  74, "bottom", "reverse", 0.5, 1.0),    # bottom-right corner -> bottom centre
#     # lap 2 (same thing, starting at LED 75)
#     (75,  86, "bottom", "reverse", 0.0, 0.5),
#     (87,  99, "left",   "reverse"),
#     (100, 123, "top",   "forward"),
#     (124, 137, "right", "forward"),
#     (138, 149, "bottom", "reverse", 0.5, 1.0),
# ]
# ---------------------------------------------------------------------------
# Your mapping (outer lap = LEDs 0-87, inner lap = 88-149), as corrected after the test.
SEGMENTS = [
    (0, 21, "bottom", "forward", 0.1154, 1.0),
    (22, 38, "right", "reverse"),
    (39, 69, "top", "reverse"),
    (70, 83, "left", "forward"),
    (84, 87, "bottom", "forward", 0.0, 0.1154),
    (88, 89, "bottom", "reverse", 0.0, 0.1333),
    (90, 101, "left", "reverse"),
    (102, 126, "top", "forward"),
    (127, 135, "right", "forward"),
    (136, 149, "bottom", "reverse", 0.1333, 1.0),
]

# ---- AUTOMATIC layout (only used when SEGMENTS = None): strip starts at a corner ----
LAPS          = 2               # how many times the strip goes around the back
LAP_LEDS      = 75              # LEDs in ONE full lap (150 LEDs / 2 laps)
START_INDEX   = 0               # LED number where lap 1 starts
LAP_STARTS    = None            # or the first LED of each lap by hand, e.g. [0, 76]
START_CORNER  = "top-left"      # top-left, top-right, bottom-right, bottom-left (seen from the front)
CLOCKWISE     = True            # direction the strip runs, seen from the front
SIDE_COUNTS   = None            # or by hand per lap, e.g. {"top": 22, "right": 16, "bottom": 22, "left": 15}

# ---- look ----
SATURATION    = 1.25            # 1.0 = natural, higher = more vivid
GAIN          = 1.0             # overall brightness multiplier of the colors
WHITE_BALANCE = (1.0, 0.92, 0.78)   # (red, green, blue) multipliers. LED strips make a bluish white; lowering blue
                                # (and a little green) makes white look white. Use 'white' to tune: 1.0 = no change
BLACK_CUTOFF  = 10              # colors dimmer than this become fully off (hides noise)
ON_EXIT_QUERY = "auto=2"        # what the strip does when you stop the script (auto=2 = chill playlist)
# =====================================================

SIDES = ["top", "right", "bottom", "left"]          # clockwise order; corners TL, TR, BR, BL
CORNERS = {"top-left": 0, "top-right": 1, "bottom-right": 2, "bottom-left": 3}


# ---------- automatic layout -> the same segment format ----------
def side_counts(lap_leds, w, h):
    """How many LEDs per side, proportional to the side length (largest remainder)."""
    if SIDE_COUNTS:
        return [int(SIDE_COUNTS[s]) for s in SIDES]
    lengths = [w, h, w, h]
    total = float(sum(lengths))
    exact = [lap_leds * L / total for L in lengths]
    counts = [int(x) for x in exact]
    order = sorted(range(4), key=lambda i: exact[i] - counts[i], reverse=True)
    for i in order[: lap_leds - sum(counts)]:
        counts[i] += 1
    return counts


def traversal(start_corner, clockwise):
    """(side index, reversed) in the order the strip visits them."""
    s = CORNERS[start_corner]
    if clockwise:
        return [((s + i) % 4, ((s + i) % 4) in (2, 3)) for i in range(4)]
    return [((s - i) % 4, ((s - i) % 4) in (0, 1)) for i in range(1, 5)]


def auto_segments(w, h):
    counts = side_counts(LAP_LEDS, w, h)
    starts = list(LAP_STARTS) if LAP_STARTS else [START_INDEX + i * LAP_LEDS for i in range(LAPS)]
    segs = []
    for st in starts:
        i = st
        for side, rev in traversal(START_CORNER, CLOCKWISE):
            c = counts[side]
            if c > 0:
                segs.append((i, i + c - 1, SIDES[side], "reverse" if rev else "forward", 0.0, 1.0))
            i += c
    return segs


def get_segments(w, h):
    """Normalise to (first, last, side, direction, from, to) tuples."""
    raw = SEGMENTS if SEGMENTS else auto_segments(w, h)
    out = []
    for s in raw:
        first, last, side, direction = s[0], s[1], s[2], s[3]
        a = float(s[4]) if len(s) > 4 else 0.0
        b = float(s[5]) if len(s) > 5 else 1.0
        out.append((int(first), int(last), side, direction, a, b))
    return out


def validate(segs):
    """Return a list of human-readable problems (empty list = fine)."""
    problems, owner = [], {}
    for k, (first, last, side, direction, a, b) in enumerate(segs):
        tag = "line %d (LEDs %d-%d, %s)" % (k + 1, first, last, side)
        if side not in SIDES:
            problems.append("%s: side must be top, right, bottom or left" % tag)
        if direction not in ("forward", "reverse"):
            problems.append("%s: direction must be \"forward\" or \"reverse\"" % tag)
        if last < first:
            problems.append("%s: last LED is smaller than first LED" % tag)
        if first < 0 or last >= NUM_LEDS:
            problems.append("%s: LED numbers must be between 0 and %d" % (tag, NUM_LEDS - 1))
        if not (0.0 <= a < b <= 1.0):
            problems.append("%s: from/to must satisfy 0 <= from < to <= 1" % tag)
        for n in range(max(first, 0), min(last, NUM_LEDS - 1) + 1):
            if n in owner and owner[n] != k:
                problems.append("%s: LED %d is already used by line %d" % (tag, n, owner[n] + 1))
                break
            owner[n] = k
    return problems


# ---------- colors ----------
def bin_means(profile, n):
    """Average a (length,3) profile into n equal bins."""
    L = profile.shape[0]
    cs = np.zeros((L + 1, 3), dtype=np.float64)
    cs[1:] = np.cumsum(profile, axis=0)
    e = np.linspace(0, L, n + 1).round().astype(int)
    e[1:] = np.maximum(e[1:], e[:-1] + 1)
    e = np.minimum(e, L)
    width = np.maximum(e[1:] - e[:-1], 1)[:, None]
    return (cs[e[1:]] - cs[e[:-1]]) / width


def frame_from_profiles(profiles, segs):
    """profiles: 4 arrays (length,3) in natural direction (top, right, bottom, left) -> (NUM_LEDS,3)."""
    frame = np.zeros((NUM_LEDS, 3), dtype=np.float32)
    for (first, last, side, direction, a, b) in segs:
        prof = profiles[SIDES.index(side)]
        L = prof.shape[0]
        lo = int(round(a * L))
        hi = max(lo + 1, int(round(b * L)))
        cols = bin_means(prof[lo:hi], last - first + 1)
        if direction == "reverse":
            cols = cols[::-1]
        frame[first:last + 1] = cols
    return frame


def tweak(frame):
    f = frame.astype(np.float32)
    if SATURATION != 1.0:
        gray = f.mean(axis=1, keepdims=True)
        f = gray + (f - gray) * SATURATION
    f = np.clip(f * GAIN * np.array(WHITE_BALANCE, dtype=np.float32), 0, 255)
    f[f.max(axis=1) < BLACK_CUTOFF] = 0
    return f.astype(np.uint8)


def test_frame(segs):
    """Top red, right green, bottom blue, left yellow; each side brightens in its FORWARD direction
    (left->right on top/bottom, top->bottom on left/right). First LED of every line is white."""
    colors = [(255, 0, 0), (0, 255, 0), (0, 80, 255), (255, 200, 0)]
    L = 400
    ramp = np.linspace(0.2, 1.0, L)[:, None]
    profiles = [np.array(c, dtype=np.float32)[None, :] * ramp for c in colors]
    frame = frame_from_profiles(profiles, segs)
    for (first, *_rest) in segs:
        if 0 <= first < NUM_LEDS:
            frame[first] = (255, 255, 255)
    return frame.astype(np.uint8)


# ---------- screen capture ----------
class ScreenSampler:
    def __init__(self, monitor_index):
        import mss
        self._mss = mss
        self._tl = threading.local()                      # mss needs one capture object per thread
        first = self._get_sct()
        self.mon = first.monitors[monitor_index]
        self.w, self.h = self.mon["width"], self.mon["height"]
        self.pool = ThreadPoolExecutor(max_workers=4) if PARALLEL_GRAB else None
        self.t_grab = self.t_proc = 0.0

    def _get_sct(self):
        sct = getattr(self._tl, "sct", None)
        if sct is None:
            make = getattr(self._mss, "MSS", None) or self._mss.mss      # newer mss versions renamed mss.mss to mss.MSS
            sct = make()
            self._tl.sct = sct
        return sct

    def grab(self, left, top, width, height):
        """Return a (rows, cols, 3) BGR array for a region given in screen points."""
        shot = self._get_sct().grab({"left": self.mon["left"] + left, "top": self.mon["top"] + top,
                                     "width": int(width), "height": int(height)})
        return np.asarray(shot)[:, :, :3]

    def profiles(self):
        """Average color along each screen side, natural direction, RGB."""
        w, h = self.w, self.h
        dh = max(1, int(h * EDGE_DEPTH))
        dw = max(1, int(w * EDGE_DEPTH))
        regions = [(0, 0, w, dh), (w - dw, 0, dw, h), (0, h - dh, w, dh), (0, 0, dw, h)]   # top, right, bottom, left
        t0 = time.perf_counter()
        pool = getattr(self, "pool", None)
        try:
            shots = list(pool.map(lambda r: self.grab(*r), regions)) if pool else [self.grab(*r) for r in regions]
        except Exception as e:
            if not pool:
                raise
            print("  (parallel capture failed: %s; going back to one edge at a time)" % e)
            self.pool = None
            shots = [self.grab(*r) for r in regions]
        t1 = time.perf_counter()
        st = max(1, int(SAMPLE_STEP))
        top, right, bottom, left = [x[::st, ::st].astype(np.float32) for x in shots]
        prof = [top.mean(axis=0), right.mean(axis=1), bottom.mean(axis=0), left.mean(axis=1)]
        out = [p[:, ::-1] for p in prof]                  # BGR -> RGB
        self.t_grab, self.t_proc = t1 - t0, time.perf_counter() - t1
        return out


# ---------- talking to the ESP32 ----------
class Esp:
    def __init__(self):
        self.ip = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def resolve(self):
        try:
            self.ip = socket.gethostbyname(ESP_HOST)
        except OSError:
            self.ip = None
        return self.ip

    def http(self, query):
        if not self.ip:
            return False
        req = urllib.request.Request("http://%s%s" % (self.ip, query))
        if WEB_PASS:
            tok = base64.b64encode(("admin:" + WEB_PASS).encode()).decode()
            req.add_header("Authorization", "Basic " + tok)
        try:
            urllib.request.urlopen(req, timeout=3).read()
            return True
        except Exception as e:
            print("  (could not reach the strip's page: %s)" % e)
            return False

    def start(self):
        if not self.resolve():
            print("Could not find '%s' on your network. Put the ESP32's IP address in ESP_HOST." % ESP_HOST)
            sys.exit(1)
        print("Strip found at", self.ip)
        if not self.http("/set?mode=20"):
            print("Could not switch the strip to Screen sync. Open its page and tap 'Screen sync (Mac)'.")

    def send(self, frame_u8):
        pkt = bytes([0xA5, NUM_LEDS]) + frame_u8.reshape(-1).tobytes()
        self.sock.sendto(pkt, (self.ip, UDP_PORT))

    def stop(self):
        if ON_EXIT_QUERY:
            self.http("/set?" + ON_EXIT_QUERY)


def serial_packet(frame_u8):
    """USB frame: A5 5A, count, R,G,B x count, then XOR checksum of the count and color bytes."""
    payload = np.ascontiguousarray(frame_u8).reshape(-1).astype(np.uint8)
    count = len(payload) // 3
    check = count ^ int(np.bitwise_xor.reduce(payload)) if len(payload) else count
    return bytes([0xA5, 0x5A, count]) + payload.tobytes() + bytes([check])


RELEASE_PACKET = bytes([0xA5, 0x5A, 0, 0])      # "script stopped": the strip goes back to its playlist


class UsbLink:
    """Sends the colors over the USB cable to the ESP32."""
    name = "USB"

    def __init__(self):
        self.ser = None
        self.sent = 0
        self.lost_reported = False

    def find_port(self):
        if SERIAL_PORT:
            return SERIAL_PORT
        import glob
        ports = []
        for pat in ("/dev/cu.usbserial*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*", "/dev/cu.usbmodem*"):
            ports += sorted(glob.glob(pat))
        ports = list(dict.fromkeys(ports))
        if len(ports) > 1 and not getattr(self, "warned", False):
            print("Several USB serial ports found: %s\nUsing the first one. Set SERIAL_PORT to choose another." % ports)
            self.warned = True
        return ports[0] if ports else None

    def open(self):
        import serial
        port = self.find_port()
        if not port:
            raise OSError("no USB serial port found (is the ESP32 plugged in with a data cable?)")
        ser = serial.Serial()
        ser.port, ser.baudrate = port, SERIAL_BAUD
        ser.dtr = False                      # keep these low so opening the port does not reset the board
        ser.rts = False
        ser.write_timeout = 0.2
        ser.open()
        self.ser = ser

    def start(self):
        try:
            self.open()
        except Exception as e:
            print("Could not open the USB link: %s" % e)
            print("Close the Arduino IDE Serial Monitor if it is open, and check the cable.")
            sys.exit(1)
        time.sleep(0.4)                      # in case the board restarted when the port opened
        print("USB link open on", self.ser.port, "at", SERIAL_BAUD, "baud")

    def send(self, frame_u8):
        import serial
        try:
            self.ser.write(serial_packet(frame_u8))
            self.lost_reported = False
            self.sent += 1
            if self.sent % 60 == 0:
                self.ser.reset_input_buffer()    # throw away anything the board printed
        except serial.SerialTimeoutException:
            pass                                 # the board was busy: skip this frame
        except (serial.SerialException, OSError):
            if not self.lost_reported:
                print("\nUSB link lost. Trying to reconnect...")
                self.lost_reported = True
            try:
                self.ser.close()
            except Exception:
                pass
            time.sleep(0.5)
            try:
                self.open()
                print("USB link back.")
            except Exception:
                pass

    def stop(self):
        try:
            for _ in range(3):
                self.ser.write(RELEASE_PACKET)
                time.sleep(0.02)
            self.ser.close()
        except Exception:
            pass


def make_link():
    link = UsbLink() if TRANSPORT == "usb" else Esp()
    link.start()
    return link


# ---------- commands ----------
def screen_size():
    try:
        s = ScreenSampler(MONITOR)
        return s.w, s.h
    except Exception:
        return 1920, 1080


def describe(segs):
    mode = "your SEGMENTS list" if SEGMENTS else "the automatic layout (%s, %s)" % (
        START_CORNER, "clockwise" if CLOCKWISE else "counter-clockwise")
    print("Mapping from %s. All directions are as seen from the FRONT of the screen.\n" % mode)
    for (first, last, side, direction, a, b) in segs:
        if side in ("top", "bottom"):
            run = "left to right" if direction == "forward" else "right to left"
        else:
            run = "top to bottom" if direction == "forward" else "bottom to top"
        part = "" if (a == 0.0 and b == 1.0) else "  [only %d%% to %d%% of the edge]" % (round(a * 100), round(b * 100))
        print("  LEDs %3d to %3d : %-6s running %s%s" % (first, last, side, run, part))
    used = sum(l - f + 1 for f, l, *_ in segs)
    print("\n%d of %d LEDs are mapped." % (used, NUM_LEDS))


def bench(segs):
    sampler = ScreenSampler(MONITOR)
    print("Screen %dx%d, EDGE_DEPTH %.2f, SAMPLE_STEP %d. Measuring 60 frames each way..." %
          (sampler.w, sampler.h, EDGE_DEPTH, SAMPLE_STEP))
    results = {}
    for parallel in (False, True):
        sampler.pool = ThreadPoolExecutor(max_workers=4) if parallel else None
        for _ in range(5):
            sampler.profiles()
        g = p = 0.0
        t0 = time.perf_counter()
        for _ in range(60):
            frame_from_profiles(sampler.profiles(), segs)
            g += sampler.t_grab
            p += sampler.t_proc
        total = (time.perf_counter() - t0) / 60
        results[parallel] = total
        print("  %-22s capture %5.1f ms   processing %4.1f ms   per frame %5.1f ms   -> up to %3.0f fps"
              % ("4 edges at once:" if parallel else "one edge at a time:", 1000 * g / 60, 1000 * p / 60, 1000 * total, 1 / total))
    best = results[True] < results[False]
    print("\nSet PARALLEL_GRAB = %s. Capture time is the main delay; to cut it further lower EDGE_DEPTH (try 0.04)." % best)


def hold(esp, frame):
    print("Showing it now. Press Ctrl+C to stop.")
    while True:
        esp.send(frame)
        time.sleep(0.05)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    w, h = screen_size() if cmd in ("info", "test", "run", "bench") else (1920, 1080)
    segs = get_segments(w, h)
    problems = validate(segs)
    if problems and cmd in ("info", "test", "run"):
        print("Problems in the mapping:")
        for p in problems:
            print("  -", p)
        print()
        if cmd in ("test", "run"):
            sys.exit(1)
    if cmd == "info":
        describe(segs)
        return
    if cmd == "bench":
        bench(segs)
        return

    esp = make_link()
    try:
        if cmd == "test":
            describe(segs)
            print("\nTop = RED, right = GREEN, bottom = BLUE, left = YELLOW.")
            print("Each side gets brighter going left->right (top/bottom) or top->bottom (left/right).")
            print("The first LED of every line above is WHITE.")
            hold(esp, test_frame(segs))
        elif cmd == "led":
            n = int(sys.argv[2])
            f = np.zeros((NUM_LEDS, 3), dtype=np.uint8)
            f[n] = (255, 255, 255)
            print("LED %d lit (white)." % n)
            hold(esp, f)
        elif cmd == "white":
            f = tweak(np.full((NUM_LEDS, 3), 200.0, dtype=np.float32))
            print("White with WHITE_BALANCE %s  ->  strip gets R,G,B = %s" % (WHITE_BALANCE, f[0].tolist()))
            print("Compare it with a white page on your screen. Too blue: lower the third number. Too green/yellow: lower the second.")
            hold(esp, f)
        elif cmd == "range":
            a, b = int(sys.argv[2]), int(sys.argv[3])
            f = np.zeros((NUM_LEDS, 3), dtype=np.uint8)
            f[a:b + 1] = (255, 255, 255)
            f[a] = (255, 0, 0)                      # first LED of the range is red
            print("LEDs %d to %d lit (white, first one red)." % (a, b))
            hold(esp, f)
        elif cmd == "scan":
            delay = float(sys.argv[2]) if len(sys.argv) > 2 else 0.35
            n = 0
            print("Lighting LEDs one at a time. Ctrl+C to stop.")
            while True:
                f = np.zeros((NUM_LEDS, 3), dtype=np.uint8)
                f[n] = (255, 255, 255)
                if n > 0:
                    f[n - 1] = (0, 60, 0)
                esp.send(f)
                print("\rLED %3d   " % n, end="", flush=True)
                time.sleep(delay)
                n = (n + 1) % NUM_LEDS
        elif cmd == "run":
            sampler = ScreenSampler(MONITOR)
            print("Screen %dx%d. Running. Press Ctrl+C to stop." % (sampler.w, sampler.h))
            period, frames, t_report, g_sum, p_sum = 1.0 / FPS, 0, time.time(), 0.0, 0.0
            while True:
                t0 = time.perf_counter()
                esp.send(tweak(frame_from_profiles(sampler.profiles(), segs)))
                frames += 1
                g_sum += sampler.t_grab
                p_sum += time.perf_counter() - t0 - sampler.t_grab
                if time.time() - t_report > 5:
                    print("  %.1f fps   (capture %.0f ms, processing+send %.0f ms per frame)"
                          % (frames / (time.time() - t_report), 1000 * g_sum / frames, 1000 * p_sum / frames))
                    frames, t_report, g_sum, p_sum = 0, time.time(), 0.0, 0.0
                left = period - (time.perf_counter() - t0)
                if left > 0:
                    time.sleep(left)
        else:
            print(__doc__)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        esp.stop()


if __name__ == "__main__":
    main()
// WiFi LED show for WS2812 + ESP32. No external libraries.
// Arduino-ESP32 core 3.x. Uses only libraries built into the ESP32 core.
//
// What it does:
//  * Joins YOUR HOME Wi-Fi, so any phone or computer on the network can open the page.
//  * Page: effects, brightness, speed, color, power, Wi-Fi radar, live paint.
//  * Shows connection status on the strip (orange = connecting, green flash = connected).
//  * If it can't join your Wi-Fi, it starts a setup hotspot where you can enter new Wi-Fi details.
//  * Wireless code updates (OTA) work over your home Wi-Fi.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFiUdp.h>

#define LED_PIN         4
#define NUM_LEDS        150
#define MAX_AMPS        0.7f    // power limiter: ~0.7 on USB, raise with a 5V adapter

// ----- Wi-Fi settings -----
#define HOME_SSID        "InkysPinky"           // your home Wi-Fi name (2.4 GHz only)
#define HOME_PASS        "inkyPinky"           // your home Wi-Fi password
#define HOSTNAME         "ledstrip"           // you can open http://ledstrip.local
#define WEB_PASS         "freeze"           // page password (user name: admin). "" = no password.
                                              // You can change it later from the page's Wi-Fi setup link.
#define OTA_PASS         "ledota123"          // password the Arduino IDE asks for when uploading wirelessly. Change it!
#define FALLBACK_AP_SSID "LED-Strip-Set"    // hotspot used only if home Wi-Fi fails
#define FALLBACK_AP_PASS "ledstrip1235094"        // at least 8 characters

// ----- Startup behaviour -----
#define START_BRIGHTNESS 0.5f   // 0.05 to 1.0 (also adjustable from the phone page)
#define PLAY_MODE        0      // auto playlist at boot: 0 = mix, 1 = relaxing, 2 = energetic
#define EFFECT_SECONDS   12     // energetic effects
#define RELAX_SECONDS    25     // relaxing effects
#define FIRE_COOL        2.5f   // higher = shorter flames
#define SYNC_PORT        4210   // UDP port the Mac script sends screen colors to
#define SYNC_SMOOTH      25.0f  // screen sync: how fast small changes follow (higher = snappier, lower = softer)
#define SYNC_SNAP        150.0f // extra speed for BIG changes, so scene cuts show up almost instantly
#define SYNC_BAUD        921600 // USB serial speed for screen sync (must match SERIAL_BAUD in ambilight.py)
#define CLU_G            0.28f  // Clu orange tint: lower = redder and darker, higher = more yellow (try 0.2 to 0.5)
#define ARES_G           0.02f  // Ares red tint: 0 = pure crimson, higher = more orange-red (try 0 to 0.12)

// Effect numbers:
//  0 Aurora   1 Lava lamp   2 Fire   3 Hyperspace   4 Ocean   5 Lightning   6 Kaleidoscope
//  7 Breathing   8 Sunset   9 Raindrops   10 Candlelight   11 Fireflies
//  12 Starry night   13 Pastel drift
//  14 Wi-Fi radar   15 Paint   16 Solid color   17 Tron Legacy (blue)   18 Tron: Clu (orange)   19 Tron: Ares (red)
//  20 Screen sync (Mac sends the screen-edge colors over USB serial or Wi-Fi UDP)

rmt_data_t ledData[NUM_LEDS * 24];
uint8_t gammaTable[256];
float buf[NUM_LEDS][3];         // float color buffer (R, G, B), 0..1

// ---------- helpers ----------
float frand(float a, float b) { return a + (b - a) * (random(10000) / 10000.0f); }
float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }

float hash01(int n) {
  uint32_t h = (uint32_t)n * 374761393u + 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFF) / 65535.0f;
}

// Smooth 1D value noise, 0..1
float vnoise(float x, int seed) {
  int i = (int)floorf(x);
  float f = x - i;
  f = f * f * (3.0f - 2.0f * f);
  float a = hash01(i + seed * 7919);
  float b = hash01(i + 1 + seed * 7919);
  return a + (b - a) * f;
}

void hueToRgb(float h, float &r, float &g, float &b) {
  h = h - floorf(h);
  float x = h * 6.0f;
  int i = (int)x;
  float f = x - i, q = 1.0f - f;
  switch (i % 6) {
    case 0: r = 1; g = f; b = 0; break;
    case 1: r = q; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = f; break;
    case 3: r = 0; g = q; b = 1; break;
    case 4: r = f; g = 0; b = 1; break;
    default: r = 1; g = 0; b = q; break;
  }
}

int putByte(int i, uint8_t value) {
  for (int bit = 7; bit >= 0; bit--) {
    bool one = (value >> bit) & 1;
    ledData[i].level0    = 1;
    ledData[i].duration0 = one ? 8 : 4;
    ledData[i].level1    = 0;
    ledData[i].duration1 = one ? 4 : 8;
    i++;
  }
  return i;
}

void addPix(int n, float r, float g, float b, float amt) {
  if (n < 0 || n >= NUM_LEDS) return;
  buf[n][0] += r * amt; buf[n][1] += g * amt; buf[n][2] += b * amt;
}

void addSmooth(float p, float r, float g, float b, float amt) {
  int n = (int)floorf(p);
  float f = p - n;
  addPix(n,     r, g, b, amt * (1.0f - f));
  addPix(n + 1, r, g, b, amt * f);
}

void fadeAll(float keep) {
  for (int n = 0; n < NUM_LEDS; n++)
    for (int c = 0; c < 3; c++) buf[n][c] *= keep;
}

void clearAll() { memset(buf, 0, sizeof(buf)); }

// =====================================================
//  ORIGINAL 7 EFFECTS (unchanged)
// =====================================================

// ---------- Effect 0: Aurora ----------
void aurora(float t) {
  for (int n = 0; n < NUM_LEDS; n++) {
    float a = vnoise(n * 0.035f + t * 0.25f, 1);
    float b = vnoise(n * 0.08f  - t * 0.40f, 2);
    float c = vnoise(n * 0.02f  + t * 0.10f, 3);
    float hue = 0.30f + 0.45f * a;               // green -> cyan -> purple
    float br = clamp01(b * b * (0.4f + 1.2f * c));
    float r, g, bl;
    hueToRgb(hue, r, g, bl);
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = bl * br;
  }
}

// ---------- Effect 1: Lava lamp ----------
void lava(float t) {
  static const float sp[5] = {0.31f, 0.23f, 0.41f, 0.17f, 0.27f};
  static const float ph[5] = {0.0f, 1.7f, 3.1f, 4.4f, 5.9f};
  float pos[5];
  for (int k = 0; k < 5; k++)
    pos[k] = (0.5f + 0.45f * sinf(t * sp[k] + ph[k])) * (NUM_LEDS - 1);
  for (int n = 0; n < NUM_LEDS; n++) {
    float f = 0;
    for (int k = 0; k < 5; k++) {
      float d = n - pos[k];
      f += expf(-d * d / 120.0f);
    }
    float hue = 0.97f + 0.15f * clamp01(f);     // magenta-red -> orange -> yellow
    float br = clamp01(0.10f + 0.90f * f);
    float r, g, b;
    hueToRgb(hue, r, g, b);
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = b * br;
  }
}

// ---------- Effect 2: Fire (from both ends) ----------
#define HALF (NUM_LEDS / 2 + 1)
float heat[HALF];

void fire(float dt) {
  for (int k = 0; k < HALF; k++) {
    heat[k] -= frand(0, 1) * FIRE_COOL * dt;
    if (heat[k] < 0) heat[k] = 0;
  }
  float mix = fminf(1.0f, dt * 35.0f);
  for (int k = HALF - 1; k >= 2; k--) {
    float target = (heat[k - 1] + heat[k - 2] * 2.0f) / 3.0f;
    heat[k] += (target - heat[k]) * mix;
  }
  if (frand(0, 1) < dt * 45.0f) {
    int k = random(0, 6);
    heat[k] = clamp01(heat[k] + frand(0.5f, 1.0f));
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    int k = min(n, NUM_LEDS - 1 - n);
    float h = heat[k];
    buf[n][0] = clamp01(h * 3.0f);
    buf[n][1] = clamp01(h * 3.0f - 1.0f);
    buf[n][2] = clamp01(h * 3.0f - 2.0f);
  }
}

// ---------- Effect 3: Hyperspace ----------
struct Star { bool on; float pos, vel, hue; };
Star stars[14];

void hyperspace(float dt, float t) {
  fadeAll(expf(-dt * 9.0f));
  if (frand(0, 1) < dt * 14.0f) {
    for (int k = 0; k < 14; k++) if (!stars[k].on) {
      stars[k].on = true;
      stars[k].pos = NUM_LEDS / 2.0f + frand(-1, 1);
      stars[k].vel = (random(2) ? 1.0f : -1.0f) * frand(4, 10);
      stars[k].hue = frand(0.5f, 0.75f);
      break;
    }
  }
  for (int k = 0; k < 14; k++) {
    if (!stars[k].on) continue;
    stars[k].vel *= 1.0f + dt * 1.8f;            // accelerate
    stars[k].pos += stars[k].vel * dt;
    if (stars[k].pos < 0 || stars[k].pos > NUM_LEDS - 1) { stars[k].on = false; continue; }
    float r, g, b;
    hueToRgb(stars[k].hue, r, g, b);
    r = r * 0.5f + 0.5f; g = g * 0.5f + 0.5f; b = b * 0.5f + 0.5f;   // pastel
    addSmooth(stars[k].pos, r, g, b, 0.6f);
  }
  float pulse = 0.5f + 0.5f * sinf(t * 3.0f);    // glowing core
  for (int n = (int)NUM_LEDS / 2 - 4; n <= (int)NUM_LEDS / 2 + 4; n++) {
    float d = n - NUM_LEDS / 2.0f;
    float v = 0.45f * pulse * expf(-d * d / 4.0f);
    buf[n][0] = fmaxf(buf[n][0], 0.6f * v);
    buf[n][1] = fmaxf(buf[n][1], 0.7f * v);
    buf[n][2] = fmaxf(buf[n][2], 1.0f * v);
  }
}

// ---------- Effect 4: Ocean ----------
void ocean(float t) {
  for (int n = 0; n < NUM_LEDS; n++) {
    float w1 = sinf(n * 0.11f  + t * 1.4f);
    float w2 = sinf(n * 0.047f - t * 0.9f);
    float w3 = sinf(n * 0.23f  + t * 2.3f);
    float h = (w1 + w2 * 1.2f + w3 * 0.4f) / 2.6f * 0.5f + 0.5f;   // 0..1
    float br = 0.12f + 0.6f * h;
    float foam = clamp01((h - 0.8f) * 5.0f);
    float r, g, b;
    hueToRgb(0.52f + 0.1f * h, r, g, b);
    buf[n][0] = clamp01(r * br + foam * 0.8f);
    buf[n][1] = clamp01(g * br + foam * 0.8f);
    buf[n][2] = clamp01(b * br + foam * 0.8f);
  }
}

// ---------- Effect 5: Lightning storm ----------
float bolt[NUM_LEDS];
float glow = 0;
int flashesLeft = 0, boltC = 0, boltLen = 0;
uint32_t nextFlash = 0;

void lightning(uint32_t now, float dt) {
  float t = now / 1000.0f;
  clearAll();
  float dk = expf(-dt * 12.0f);
  for (int n = 0; n < NUM_LEDS; n++) bolt[n] *= dk;
  glow *= expf(-dt * 5.0f);

  if (flashesLeft == 0 && frand(0, 1) < dt * 0.7f) {   // new strike about every 1.4 s
    flashesLeft = random(2, 5);
    nextFlash = now;
    boltC = random(10, NUM_LEDS - 10);
    boltLen = random(8, 40);
  }
  if (flashesLeft > 0 && now >= nextFlash) {
    flashesLeft--;
    nextFlash = now + random(40, 110);
    for (int n = boltC - boltLen / 2; n <= boltC + boltLen / 2; n++)
      if (n >= 0 && n < NUM_LEDS && random(100) < 85) bolt[n] = frand(0.7f, 1.0f);
    glow = fmaxf(glow, 0.5f);
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    float bg = 0.04f + 0.08f * vnoise(n * 0.05f + t * 0.3f, 5);
    float l = bolt[n] + glow * 0.25f;
    buf[n][0] = clamp01(bg * 0.5f + l * 0.8f);
    buf[n][1] = clamp01(bg * 0.1f + l * 0.85f);
    buf[n][2] = clamp01(bg + l);
  }
}

// ---------- Effect 6: Kaleidoscope ----------
void kaleido(float t) {
  float c = (NUM_LEDS - 1) / 2.0f;
  for (int n = 0; n < NUM_LEDS; n++) {
    float d = fabsf(n - c);
    float hue = d * 0.012f - t * 0.25f + 0.15f * sinf(t * 0.7f);
    float br = 0.5f + 0.5f * sinf(d * 0.35f - t * 5.0f);
    br = br * br;
    float r, g, b;
    hueToRgb(hue, r, g, b);
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = b * br;
  }
}

// =====================================================
//  NEW RELAXING EFFECTS
// =====================================================

// ---------- Effect 7: Breathing ----------
// Whole strip slowly swells and fades (8 s per breath), color drifts blue -> violet -> pink.
// A tiny phase offset along the strip makes the breath ripple gently.
void breathing(float t) {
  for (int n = 0; n < NUM_LEDS; n++) {
    float ph = t * (6.2832f / 8.0f) - n * 0.012f;
    float b = 0.5f - 0.5f * cosf(ph);              // 0..1
    b = b * b * (3.0f - 2.0f * b);                 // softer ends
    float br = 0.03f + 0.97f * b;
    float hue = 0.62f + 0.16f * sinf(t * 0.11f + n * 0.01f);
    float r, g, bl;
    hueToRgb(hue, r, g, bl);
    r = r * 0.85f + 0.15f; g = g * 0.85f + 0.15f; bl = bl * 0.85f + 0.15f;   // slightly soft
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = bl * br;
  }
}

// ---------- Effect 8: Sunset ----------
// Slow drifting gradient of orange, rose and purple with gentle brightness clouds.
void sunset(float t) {
  const float c0[3] = {1.0f, 0.30f, 0.00f};   // orange
  const float c1[3] = {1.0f, 0.06f, 0.30f};   // rose
  const float c2[3] = {0.35f, 0.00f, 0.80f};  // purple
  for (int n = 0; n < NUM_LEDS; n++) {
    float x = n / (float)NUM_LEDS;
    float s = 0.5f + 0.5f * sinf(x * 3.0f + t * 0.12f + vnoise(n * 0.02f + t * 0.05f, 31) * 1.5f);
    float r, g, b;
    if (s < 0.5f) {
      float k = s * 2.0f;
      r = mixf(c0[0], c1[0], k); g = mixf(c0[1], c1[1], k); b = mixf(c0[2], c1[2], k);
    } else {
      float k = (s - 0.5f) * 2.0f;
      r = mixf(c1[0], c2[0], k); g = mixf(c1[1], c2[1], k); b = mixf(c1[2], c2[2], k);
    }
    float br = 0.55f + 0.45f * vnoise(n * 0.03f + t * 0.08f, 32);
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = b * br;
  }
}

// ---------- Effect 9: Raindrops ----------
// Soft blue ripples expanding from random drops on a dim blue "pond".
struct Drop { bool on; float c, r; };
Drop drops[8];

void raindrops(float dt) {
  clearAll();
  for (int n = 0; n < NUM_LEDS; n++) { buf[n][1] = 0.02f; buf[n][2] = 0.05f; }
  if (frand(0, 1) < dt * 2.5f) {
    for (int k = 0; k < 8; k++) if (!drops[k].on) {
      drops[k] = { true, frand(5, NUM_LEDS - 5), 0 };
      break;
    }
  }
  for (int k = 0; k < 8; k++) {
    if (!drops[k].on) continue;
    drops[k].r += 11.0f * dt;
    float life = 1.0f - drops[k].r / 20.0f;
    if (life <= 0) { drops[k].on = false; continue; }
    int lo = max(0, (int)(drops[k].c - drops[k].r - 5));
    int hi = min(NUM_LEDS - 1, (int)(drops[k].c + drops[k].r + 5));
    for (int n = lo; n <= hi; n++) {
      float d = fabsf(n - drops[k].c) - drops[k].r;
      float w = expf(-(d * d) / 3.0f) * life * life * 0.7f;
      addPix(n, 0.2f, 0.6f, 1.0f, w);
    }
  }
}

// ---------- Effect 10: Candlelight ----------
// Warm, slowly flickering glow with soft moving pools of light.
void candle(float t) {
  float global = 0.15f * vnoise(t * 6.0f, 41);
  for (int n = 0; n < NUM_LEDS; n++) {
    float pool = vnoise(n * 0.12f + t * 1.2f, 42);
    float fl   = vnoise(n * 0.25f - t * 3.0f, 43);
    float br = clamp01(0.30f + 0.40f * pool + 0.15f * fl + global);
    buf[n][0] = br;
    buf[n][1] = br * (0.30f + 0.12f * br);
    buf[n][2] = br * 0.03f;
  }
}

// ---------- Effect 11: Fireflies ----------
// Twelve soft yellow-green lights that drift and slowly blink.
struct Fly { float pos, vel, phase, rate; };
Fly flies[12];

void initFlies() {
  for (int k = 0; k < 12; k++)
    flies[k] = { frand(0, NUM_LEDS - 1), frand(-2, 2), frand(0, 6.28f), frand(0.8f, 2.0f) };
}

void fireflies(float dt) {
  clearAll();
  for (int n = 0; n < NUM_LEDS; n++) { buf[n][1] = 0.015f; buf[n][2] = 0.004f; }
  for (int k = 0; k < 12; k++) {
    Fly &f = flies[k];
    f.vel += frand(-1, 1) * dt * 8.0f;
    if (f.vel > 5) f.vel = 5;
    if (f.vel < -5) f.vel = -5;
    f.pos += f.vel * dt;
    if (f.pos < 0)            { f.pos = 0;            f.vel = fabsf(f.vel); }
    if (f.pos > NUM_LEDS - 1) { f.pos = NUM_LEDS - 1; f.vel = -fabsf(f.vel); }
    f.phase += dt * f.rate;
    float s = sinf(f.phase);
    if (s < 0) s = 0;
    float b = s * s * s;
    if (b > 0.01f) {
      for (int n = (int)f.pos - 3; n <= (int)f.pos + 3; n++) {
        float d = n - f.pos;
        addPix(n, 0.75f, 1.0f, 0.10f, b * expf(-d * d / 1.8f) * 0.9f);
      }
    }
  }
}

// ---------- Effect 12: Starry night ----------
// Dim deep-blue sky, slowly twinkling stars, and an occasional gentle shooting star.
bool meteorOn = false;
float meteorPos = 0, meteorDir = 1;

void starry(float dt, float t) {
  for (int n = 0; n < NUM_LEDS; n++) {
    float sky = 0.025f + 0.035f * vnoise(n * 0.04f + t * 0.05f, 51);
    float r = sky * 0.4f, g = sky * 0.6f, b = sky;
    if (hash01(n * 13 + 5) < 0.14f) {                      // this LED holds a star
      float rate = 0.3f + hash01(n * 7 + 1) * 0.8f;
      float ph = hash01(n * 31 + 2) * 6.2832f;
      float tw = 0.5f + 0.5f * sinf(t * rate + ph);
      tw = tw * tw * tw;
      r += 0.75f * tw; g += 0.85f * tw; b += 1.0f * tw;
    }
    buf[n][0] = r; buf[n][1] = g; buf[n][2] = b;
  }
  if (!meteorOn && frand(0, 1) < dt * 0.12f) {
    meteorOn = true;
    meteorDir = random(2) ? 1.0f : -1.0f;
    meteorPos = meteorDir > 0 ? 0.0f : NUM_LEDS - 1.0f;
  }
  if (meteorOn) {
    meteorPos += meteorDir * 55.0f * dt;
    if (meteorPos < -10 || meteorPos > NUM_LEDS + 10) meteorOn = false;
    for (int d = 0; d < 16; d++)
      addSmooth(meteorPos - meteorDir * d, 0.8f, 0.9f, 1.0f, 0.7f * expf(-d / 3.5f));
  }
}

// ---------- Effect 13: Pastel drift ----------
// Slow, soft pastel rainbow that wanders along the strip.
void pastel(float t) {
  for (int n = 0; n < NUM_LEDS; n++) {
    float hue = t * 0.02f + n * 0.004f + 0.12f * vnoise(n * 0.03f + t * 0.2f, 61);
    float r, g, b;
    hueToRgb(hue, r, g, b);
    r = r * 0.55f + 0.45f; g = g * 0.55f + 0.45f; b = b * 0.55f + 0.45f;   // mix with white
    float br = 0.50f + 0.30f * vnoise(n * 0.05f - t * 0.15f, 62);
    buf[n][0] = r * br; buf[n][1] = g * br; buf[n][2] = b * br;
  }
}

// =====================================================
//  WIFI-POWERED EFFECTS
// =====================================================

int effect = 0;            // currently running effect

void addBlob(float center, float sigma, float r, float g, float b, float amt) {
  int lo = max(0, (int)(center - 4 * sigma));
  int hi = min(NUM_LEDS - 1, (int)(center + 4 * sigma) + 1);
  for (int n = lo; n <= hi; n++) {
    float d = n - center;
    float w = expf(-(d * d) / (2 * sigma * sigma)) * amt;
    addPix(n, r, g, b, w);
  }
}

// ---------- Effect 14: Wi-Fi radar ----------
// Every nearby network is a glowing blob. Position = Wi-Fi channel (1 to 13),
// size and brightness = signal strength, red = open network, other colors = secured.
// A green radar beam sweeps along the strip, and each network pings as it passes.
#define MAX_NETS 20
struct Net { String ssid; int rssi; int ch; bool secured; uint16_t id; };
Net nets[MAX_NETS];
int netCount = 0;
float blip[MAX_NETS];
float sweepPrev = 0;
uint32_t lastScan = 0;
bool wantScan = false, scanFresh = false;

float netPos(int k) {
  int ch = constrain(nets[k].ch, 1, 13);
  float p = 15.0f + (ch - 1) / 12.0f * (NUM_LEDS - 30) + (hash01(nets[k].id) - 0.5f) * 10.0f;
  return constrain(p, 2.0f, NUM_LEDS - 3.0f);
}

void scanTick(uint32_t now) {
  int n = WiFi.scanComplete();
  if (n >= 0) {                                   // a scan just finished
    netCount = min(n, MAX_NETS);
    for (int i = 0; i < netCount; i++) {
      nets[i].ssid = WiFi.SSID(i);
      nets[i].rssi = WiFi.RSSI(i);
      nets[i].ch = WiFi.channel(i);
      nets[i].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      uint8_t *b = WiFi.BSSID(i);
      nets[i].id = b ? ((b[4] << 8) | b[5]) : i;
    }
    WiFi.scanDelete();
    lastScan = now;
    scanFresh = true;
  } else if (n == -2) {                           // idle: start a new scan if needed
    if (wantScan || (effect == 14 && (now - lastScan) > 7000)) {
      WiFi.scanNetworks(true, true);              // async, include hidden networks
      wantScan = false;
    }
  }
}

void radar(float t, float dt) {
  clearAll();
  if (netCount == 0) {                            // waiting for the first scan
    float p = 0.5f + 0.5f * sinf(t * 2.0f);
    for (int n = 0; n < NUM_LEDS; n++) { buf[n][1] = 0.05f * p; buf[n][2] = 0.08f * p; }
  }
  float sweep = fmodf(t * 45.0f, (float)NUM_LEDS);
  bool wrapped = sweep < sweepPrev;
  for (int k = 0; k < netCount; k++) {
    float pos = netPos(k);
    bool hit = wrapped ? (pos >= sweepPrev || pos < sweep) : (pos >= sweepPrev && pos < sweep);
    if (hit) blip[k] = 1.0f;
  }
  sweepPrev = sweep;

  for (int d = 0; d < 25; d++)                    // radar beam with a fading tail
    addSmooth(sweep - d, 0.1f, 1.0f, 0.3f, 0.55f * expf(-d / 5.0f));

  for (int k = 0; k < netCount; k++) {
    blip[k] *= expf(-dt * 0.9f);
    float s = clamp01((nets[k].rssi + 95) / 55.0f);   // -95 dBm = 0, -40 dBm = 1
    float sigma = 1.0f + 2.5f * s;
    float r, g, b;
    if (!nets[k].secured) { r = 1.0f; g = 0.1f; b = 0.1f; }
    else hueToRgb(0.5f + (constrain(nets[k].ch, 1, 13) - 1) * 0.04f, r, g, b);
    float pulse = 0.7f + 0.3f * sinf(t * 2.0f + k * 1.7f);
    float amt = (0.10f + 0.30f * s) * pulse + blip[k] * (0.5f + 0.5f * s);
    addBlob(netPos(k), sigma, r, g, b, amt);
  }
}

// ---------- Effect 15: Paint (drawn live from the phone) ----------
float paintBuf[NUM_LEDS][3];
bool paintFade = false;

void paintEffect(float dt) {
  if (paintFade) {
    float k = expf(-dt * 0.5f);
    for (int n = 0; n < NUM_LEDS; n++)
      for (int c = 0; c < 3; c++) paintBuf[n][c] *= k;
  }
  memcpy(buf, paintBuf, sizeof(buf));
}

// ---------- Effect 16: Solid color ----------
uint8_t colR = 255, colG = 80, colB = 0;

void solidColor() {
  for (int n = 0; n < NUM_LEDS; n++) {
    buf[n][0] = colR / 255.0f; buf[n][1] = colG / 255.0f; buf[n][2] = colB / 255.0f;
  }
}

// ---------- Effects 17 and 18: Tron Legacy (blue) and Tron: Clu (orange) ----------
// Black grid with a faint circuit pattern, light cycles leaving glowing trails, data pulses
// racing along the grid, and a flash when two cycles collide.
// 17 = blue and ice-white (the heroes).
// (18 = Clu orange has its own function, tronClu, and 19 = Ares red has tronAres, further below.)
struct Cycle { float pos, vel, turnIn; };
Cycle cycles[4];
float trail[NUM_LEDS];
float pulsePos = -1000, pulseDir = 1, tronFlash = 0, flashPos = 0;

// Colors as {red, green, blue}, 0..1
struct Pal { float grid[3], trail[3], core[3], head[3], pulse[3], pulseHead[3], flash[3]; };
const Pal PAL_BLUE = {{0, 0.6f, 1}, {0, 0.8f, 1}, {0.55f, 0, 0},      {0.8f, 0.97f, 1}, {0.1f, 0.7f, 1},  {0.5f, 0.9f, 1},  {0.6f, 0.9f, 1}};
const Pal PAL_RED  = {{1, 0.02f, 0.03f}, {1, 0.05f, 0.04f}, {0, 0.40f, 0.35f}, {1, 0.60f, 0.55f}, {1, 0.10f, 0.05f}, {1, 0.50f, 0.40f}, {1, 0.30f, 0.25f}};
const Pal PAL_CLU  = {{1, 0.3f, 0}, {1, 0.38f, 0}, {0, 0.45f, 0.25f}, {1, 0.8f, 0.45f}, {1, 0.4f, 0.05f}, {1, 0.7f, 0.3f}, {1, 0.6f, 0.2f}};

void initTron(bool clu) {      // clu = true gives the aggressive behavior (orange and red)
  float lo = clu ? 60.0f : 40.0f, hi = clu ? 130.0f : 90.0f;
  for (int k = 0; k < 4; k++)
    cycles[k] = { frand(0, NUM_LEDS - 1), (random(2) ? 1.0f : -1.0f) * frand(lo, hi), frand(0.5f, 1.5f) };
  memset(trail, 0, sizeof(trail));
  pulsePos = -1000;
  tronFlash = 0;
}

void tron(float t, float dt, int pal, bool clu) {   // pal: 0 = blue, 1 = orange, 2 = red
  const Pal &P = (pal == 0) ? PAL_BLUE : (pal == 1 ? PAL_CLU : PAL_RED);

  // faint circuit grid: brighter node every 10th LED
  for (int n = 0; n < NUM_LEDS; n++) {
    float node = (n % 10 == 0) ? 0.12f : 0.025f;
    float br = node * (0.7f + 0.3f * sinf(t * 0.9f + n * 0.07f));
    for (int c = 0; c < 3; c++) buf[n][c] = P.grid[c] * br;
  }

  // trails fade slowly (Clu's linger a little longer)
  float keep = expf(-dt * (clu ? 1.2f : 1.6f));
  for (int n = 0; n < NUM_LEDS; n++) trail[n] *= keep;

  // move the light cycles; they "turn" (reverse) at random and at the ends
  for (int k = 0; k < 4; k++) {
    Cycle &c = cycles[k];
    c.pos += c.vel * dt;
    c.turnIn -= dt;
    if (c.pos < 0 || c.pos > NUM_LEDS - 1 || c.turnIn <= 0) {
      c.pos = fmaxf(0.0f, fminf(c.pos, NUM_LEDS - 1.0f));
      c.vel = (c.vel > 0 ? -1.0f : 1.0f) * (clu ? frand(60, 140) : frand(40, 100));
      c.turnIn = clu ? frand(0.5f, 1.5f) : frand(0.8f, 2.5f);
    }
  }

  // collisions: bright flash, both cycles bounce back
  for (int j = 0; j < 4; j++)
    for (int k = j + 1; k < 4; k++)
      if (fabsf(cycles[j].pos - cycles[k].pos) < 1.2f && cycles[j].vel * cycles[k].vel < 0 && tronFlash < 0.3f) {
        tronFlash = 1.0f;
        flashPos = (cycles[j].pos + cycles[k].pos) / 2.0f;
        cycles[j].vel = -cycles[j].vel;
        cycles[k].vel = -cycles[k].vel;
      }

  // leave glowing trail behind each cycle
  for (int k = 0; k < 4; k++) {
    int n = (int)floorf(cycles[k].pos);
    float f = cycles[k].pos - n;
    if (n >= 0 && n < NUM_LEDS)         trail[n]     = fmaxf(trail[n],     1.0f - f);
    if (n + 1 >= 0 && n + 1 < NUM_LEDS) trail[n + 1] = fmaxf(trail[n + 1], f);
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    float tr = trail[n];
    float hot = tr * tr * tr;                    // hot core near the head
    for (int c = 0; c < 3; c++) buf[n][c] += P.trail[c] * tr + P.core[c] * hot;
  }
  for (int k = 0; k < 4; k++)
    addBlob(cycles[k].pos, 0.9f, P.head[0], P.head[1], P.head[2], 1.0f);

  // data pulse racing along the grid every few seconds
  if (pulsePos < -500 && frand(0, 1) < dt * (clu ? 0.35f : 0.2f)) {
    pulseDir = random(2) ? 1.0f : -1.0f;
    pulsePos = (pulseDir > 0) ? -3.0f : NUM_LEDS + 2.0f;
  }
  if (pulsePos > -500) {
    pulsePos += pulseDir * (clu ? 180.0f : 140.0f) * dt;
    if (pulsePos < -10 || pulsePos > NUM_LEDS + 10) pulsePos = -1000;
    else {
      for (int d = 0; d < 20; d++)
        addSmooth(pulsePos - pulseDir * d, P.pulse[0], P.pulse[1], P.pulse[2], 0.5f * expf(-d / 5.0f));
      addBlob(pulsePos, 1.0f, P.pulseHead[0], P.pulseHead[1], P.pulseHead[2], 0.8f);
    }
  }

  // collision flash
  if (tronFlash > 0) {
    addBlob(flashPos, clu ? 8.0f : 6.0f, P.flash[0], P.flash[1], P.flash[2], tronFlash * 1.2f);
    tronFlash *= expf(-dt * 5.0f);
    if (tronFlash < 0.01f) tronFlash = 0;
  }
}

// ---------- Effect 18: Tron: Clu (orange) ----------
// Clu's army: disciplined and relentless. A dark grid with a steady marching pulse of nodes,
// four cycles that move in formation and converge on the center, crisp light ribbons, a slow
// advancing "purge" wall that sweeps the whole grid, and derezz: when cycles collide, one
// shatters into voxels and respawns from an end of the strip.
struct Voxel { float pos, vel, life; };
Voxel vox[24];
float deadFor[4];

void initClu() {
  // two cycles from each end, heading inward in formation
  for (int k = 0; k < 4; k++) {
    bool left = (k < 2);
    float off = frand(0, 40);
    cycles[k] = { left ? off : (NUM_LEDS - 1 - off), (left ? 1.0f : -1.0f) * frand(75, 95), frand(1.5f, 4.0f) };
    deadFor[k] = 0;
  }
  memset(trail, 0, sizeof(trail));
  memset(vox, 0, sizeof(vox));
  pulsePos = -1000;
  tronFlash = 0;
}

void spawnVoxels(float pos) {
  int made = 0;
  for (int i = 0; i < 24 && made < 14; i++) {
    if (vox[i].life > 0) continue;
    vox[i].pos = pos + frand(-1.0f, 1.0f);
    vox[i].vel = (random(2) ? 1.0f : -1.0f) * frand(15, 60);
    vox[i].life = frand(0.7f, 1.3f);
    made++;
  }
}

float wallPos = -1000, wallDir = 1, wallIn = 4.0f;

void tronClu(float t, float dt) {
  const float G = CLU_G;

  // dark grid: dim lines, with a steady marching pulse of nodes like a command lattice
  int step = (int)(t * 5.0f);
  float breathe = 0.75f + 0.25f * sinf(t * 0.5f);
  for (int n = 0; n < NUM_LEDS; n++) {
    float br = (n % 10 == 0) ? 0.06f : 0.012f;
    if (n % 10 == 0 && ((n / 10 + step) % 6) == 0) br = 0.30f;          // marching node
    br *= breathe;
    buf[n][0] = br; buf[n][1] = G * 0.8f * br; buf[n][2] = 0;
  }

  // light ribbons fade in a straight line (crisp, not a soft glow)
  for (int n = 0; n < NUM_LEDS; n++) trail[n] = fmaxf(0.0f, trail[n] - dt * 0.9f);

  // cycles: steady speed, turn only at the ends or at grid nodes, mostly toward the center
  for (int k = 0; k < 4; k++) {
    if (deadFor[k] > 0) {                              // derezzed: wait, then respawn at its end
      deadFor[k] -= dt;
      if (deadFor[k] <= 0) {
        bool left = (k < 2);
        cycles[k].pos = left ? 0.0f : NUM_LEDS - 1.0f;
        cycles[k].vel = (left ? 1.0f : -1.0f) * frand(75, 95);
        cycles[k].turnIn = frand(1.5f, 4.0f);
      }
      continue;
    }
    Cycle &c = cycles[k];
    float prev = c.pos;
    c.pos += c.vel * dt;
    c.turnIn -= dt;
    if (c.pos < 0 || c.pos > NUM_LEDS - 1) {
      c.pos = fmaxf(0.0f, fminf(c.pos, NUM_LEDS - 1.0f));
      c.vel = -c.vel;
      c.turnIn = frand(1.5f, 4.0f);
    } else if (c.turnIn <= 0 && (int)(prev / 10) != (int)(c.pos / 10)) {   // crossed a grid node
      if (random(100) < 60) c.vel = ((c.pos < (NUM_LEDS - 1) / 2.0f) ? 1.0f : -1.0f) * fabsf(c.vel);   // converge
      else c.vel = -c.vel;
      c.turnIn = frand(1.5f, 4.0f);
    }
  }

  // collision: one cycle derezzes into a burst of voxels
  for (int j = 0; j < 4; j++)
    for (int k = j + 1; k < 4; k++) {
      if (deadFor[j] > 0 || deadFor[k] > 0) continue;
      if (fabsf(cycles[j].pos - cycles[k].pos) < 1.5f && cycles[j].vel * cycles[k].vel < 0 && random(100) < 15) {   // only some contacts are fatal
        int d = random(2) ? j : k;
        int sv = (d == j) ? k : j;
        spawnVoxels(cycles[d].pos);
        spawnVoxels(cycles[d].pos);
        flashPos = cycles[d].pos;
        tronFlash = 1.0f;
        deadFor[d] = frand(2.5f, 4.0f);   // stays gone for a few seconds
        cycles[sv].vel = -cycles[sv].vel;
      }
    }

  // lay down the light ribbons
  for (int k = 0; k < 4; k++) {
    if (deadFor[k] > 0) continue;
    int n = (int)floorf(cycles[k].pos);
    float f = cycles[k].pos - n;
    if (n >= 0 && n < NUM_LEDS)         trail[n]     = fmaxf(trail[n],     1.0f - f);
    if (n + 1 >= 0 && n + 1 < NUM_LEDS) trail[n + 1] = fmaxf(trail[n + 1], f);
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    float a = 0.6f * trail[n];
    buf[n][0] += a; buf[n][1] += G * a;
  }

  // small, tight, orange heads (no white)
  for (int k = 0; k < 4; k++)
    if (deadFor[k] <= 0) addBlob(cycles[k].pos, 0.6f, 1.0f, fminf(1.0f, G * 1.6f), 0.04f, 1.0f);

  // derezz voxels: single LEDs that scatter, slow down and flicker out
  for (int i = 0; i < 24; i++) {
    if (vox[i].life <= 0) continue;
    vox[i].pos += vox[i].vel * dt;
    vox[i].vel *= expf(-dt * 2.0f);
    vox[i].life -= dt;
    if (random(100) < 25) continue;                    // flicker
    addPix((int)roundf(vox[i].pos), 1.0f, fminf(1.0f, G * 1.4f), 0.03f, clamp01(vox[i].life));
  }

  // the purge: a slow, relentless wall sweeps the whole grid every 7 to 12 seconds
  wallIn -= dt;
  if (wallPos < -500 && wallIn <= 0) {
    wallDir = random(2) ? 1.0f : -1.0f;
    wallPos = (wallDir > 0) ? -2.0f : NUM_LEDS + 1.0f;
    wallIn = frand(7.0f, 12.0f);
  }
  if (wallPos > -500) {
    wallPos += wallDir * 45.0f * dt;
    if (wallPos < -30 || wallPos > NUM_LEDS + 30) wallPos = -1000;
    else {
      for (int d = 0; d < 26; d++)                     // dim wash behind a hard bright edge
        addSmooth(wallPos - wallDir * d, 1.0f, G, 0.0f, 0.30f * (1.0f - d / 26.0f));
      addBlob(wallPos, 0.5f, 1.0f, fminf(1.0f, G * 1.4f), 0.03f, 1.0f);
    }
  }

  // data pulse: sharp head, short tail
  if (pulsePos < -500 && frand(0, 1) < dt * 0.25f) {
    pulseDir = random(2) ? 1.0f : -1.0f;
    pulsePos = (pulseDir > 0) ? -3.0f : NUM_LEDS + 2.0f;
  }
  if (pulsePos > -500) {
    pulsePos += pulseDir * 170.0f * dt;
    if (pulsePos < -10 || pulsePos > NUM_LEDS + 10) pulsePos = -1000;
    else {
      for (int d = 0; d < 10; d++)
        addSmooth(pulsePos - pulseDir * d, 1.0f, G * 1.2f, 0.02f, 0.55f * expf(-d / 3.0f));
      addBlob(pulsePos, 0.7f, 1.0f, fminf(1.0f, G * 1.5f), 0.05f, 0.9f);
    }
  }

  // flash where a cycle derezzed
  if (tronFlash > 0) {
    addBlob(flashPos, 7.0f, 1.0f, fminf(1.0f, G * 1.3f), 0.05f, tronFlash);
    tronFlash *= expf(-dt * 5.0f);
    if (tronFlash < 0.01f) tronFlash = 0;
  }

  // where lights overlap, scale down instead of clipping, so the orange never washes out
  for (int n = 0; n < NUM_LEDS; n++) {
    float m = fmaxf(buf[n][0], fmaxf(buf[n][1], buf[n][2]));
    if (m > 1.0f) { buf[n][0] /= m; buf[n][1] /= m; buf[n][2] /= m; }
  }
}

// ---------- Effect 19: Tron: Ares (red) ----------
// Menacing, not pretty: pure crimson on near-black, a slow heartbeat in the grid, a red
// searchlight sweeping like a Recognizer's eye, hunter cycles that chase each other and lunge,
// brutal derezz bursts, and digital glitch blocks that flash or cut to black.
float glitchIn = 2.0f, glitchLeft = 0;
int glitchStart = 0, glitchLen = 0, glitchKind = 0;

void initAres() {
  for (int k = 0; k < 4; k++) {
    cycles[k] = { frand(0, NUM_LEDS - 1), (random(2) ? 1.0f : -1.0f) * frand(70, 110), frand(0.8f, 2.0f) };
    deadFor[k] = 0;
  }
  memset(trail, 0, sizeof(trail));
  memset(vox, 0, sizeof(vox));
  pulsePos = -1000;
  tronFlash = 0;
  glitchIn = 2.0f; glitchLeft = 0;
}

void tronAres(float t, float dt) {
  const float G = ARES_G;

  // slow heartbeat (lub-dub) makes the whole grid throb
  float p = fmodf(t * 0.9f, 1.0f);
  float a1 = (p - 0.15f) / 0.06f, a2 = (p - 0.37f) / 0.06f;
  float hb = expf(-a1 * a1) + 0.7f * expf(-a2 * a2);
  for (int n = 0; n < NUM_LEDS; n++) {
    float node = (n % 10 == 0) ? 0.14f : 0.025f;
    float br = node * (0.45f + 1.1f * hb);
    buf[n][0] = br; buf[n][1] = G * br; buf[n][2] = 0.02f * br;
  }

  // searchlight: a wide dim red band with a hard core, drifting back and forth
  float sp = (0.5f + 0.5f * sinf(t * 0.4f)) * (NUM_LEDS - 1);
  for (int n = 0; n < NUM_LEDS; n++) {
    float d = n - sp;
    float w = 0.10f * expf(-d * d / 128.0f) + 0.45f * expf(-d * d / 2.8f);
    buf[n][0] += w; buf[n][1] += G * w; buf[n][2] += 0.02f * w;
  }

  // light ribbons fade in a straight line
  for (int n = 0; n < NUM_LEDS; n++) trail[n] = fmaxf(0.0f, trail[n] - dt * 1.1f);

  // hunter cycles: lunge in bursts, and at grid nodes turn toward the nearest rival
  for (int k = 0; k < 3; k++) {
    if (deadFor[k] > 0) {
      deadFor[k] -= dt;
      if (deadFor[k] <= 0) {
        bool left = random(2);
        cycles[k].pos = left ? 0.0f : NUM_LEDS - 1.0f;
        cycles[k].vel = (left ? 1.0f : -1.0f) * frand(70, 110);
        cycles[k].turnIn = frand(0.8f, 2.0f);
      }
      continue;
    }
    Cycle &c = cycles[k];
    float lunge = 0.7f + 0.8f * powf(0.5f + 0.5f * sinf(t * 3.2f + k * 2.1f), 3.0f);   // 0.7x to 1.5x
    float prev = c.pos;
    c.pos += c.vel * lunge * dt;
    c.turnIn -= dt;
    if (c.pos < 0 || c.pos > NUM_LEDS - 1) {
      c.pos = fmaxf(0.0f, fminf(c.pos, NUM_LEDS - 1.0f));
      c.vel = -c.vel;
      c.turnIn = frand(0.8f, 2.0f);
    } else if (c.turnIn <= 0 && (int)(prev / 10) != (int)(c.pos / 10)) {
      int best = -1; float bd = 1e9f;
      for (int j = 0; j < 3; j++) {
        if (j == k || deadFor[j] > 0) continue;
        float d = fabsf(cycles[j].pos - c.pos);
        if (d < bd) { bd = d; best = j; }
      }
      if (best >= 0) c.vel = ((cycles[best].pos > c.pos) ? 1.0f : -1.0f) * fabsf(c.vel);   // hunt
      else c.vel = -c.vel;
      c.turnIn = frand(0.8f, 2.0f);
    }
  }

  // contact: one cycle is derezzed in a violent burst, with a glitch
  for (int j = 0; j < 3; j++)
    for (int k = j + 1; k < 3; k++) {
      if (deadFor[j] > 0 || deadFor[k] > 0) continue;
      if (fabsf(cycles[j].pos - cycles[k].pos) < 1.6f) {
        int d = random(2) ? j : k;
        spawnVoxels(cycles[d].pos);
        spawnVoxels(cycles[d].pos);
        flashPos = cycles[d].pos;
        tronFlash = 1.0f;
        deadFor[d] = frand(1.2f, 2.5f);
        glitchLeft = 0.3f;
        glitchStart = max(0, (int)cycles[d].pos - 15);
        glitchLen = 30;
        glitchKind = random(2);
      }
    }

  // lay down ribbons
  for (int k = 0; k < 3; k++) {
    if (deadFor[k] > 0) continue;
    int n = (int)floorf(cycles[k].pos);
    float f = cycles[k].pos - n;
    if (n >= 0 && n < NUM_LEDS)         trail[n]     = fmaxf(trail[n],     1.0f - f);
    if (n + 1 >= 0 && n + 1 < NUM_LEDS) trail[n + 1] = fmaxf(trail[n + 1], f);
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    float a = 0.7f * trail[n];
    buf[n][0] += a; buf[n][1] += G * a; buf[n][2] += 0.02f * a;
  }

  // heads: tight, bright red with only a hint of heat
  for (int k = 0; k < 3; k++)
    if (deadFor[k] <= 0) addBlob(cycles[k].pos, 0.7f, 1.0f, 0.10f + G, 0.06f, 1.0f);

  // voxels from derezzed cycles
  for (int i = 0; i < 24; i++) {
    if (vox[i].life <= 0) continue;
    vox[i].pos += vox[i].vel * dt;
    vox[i].vel *= expf(-dt * 1.5f);
    vox[i].life -= dt;
    if (random(100) < 25) continue;
    addPix((int)roundf(vox[i].pos), 1.0f, G + 0.08f, 0.04f, clamp01(vox[i].life));
  }

  // fast red data pulse
  if (pulsePos < -500 && frand(0, 1) < dt * 0.3f) {
    pulseDir = random(2) ? 1.0f : -1.0f;
    pulsePos = (pulseDir > 0) ? -3.0f : NUM_LEDS + 2.0f;
  }
  if (pulsePos > -500) {
    pulsePos += pulseDir * 200.0f * dt;
    if (pulsePos < -10 || pulsePos > NUM_LEDS + 10) pulsePos = -1000;
    else {
      for (int d = 0; d < 12; d++)
        addSmooth(pulsePos - pulseDir * d, 1.0f, G, 0.02f, 0.55f * expf(-d / 3.0f));
      addBlob(pulsePos, 0.7f, 1.0f, G + 0.05f, 0.03f, 0.9f);
    }
  }

  // derezz flash
  if (tronFlash > 0) {
    addBlob(flashPos, 7.0f, 1.0f, G + 0.05f, 0.03f, tronFlash);
    tronFlash *= expf(-dt * 5.0f);
    if (tronFlash < 0.01f) tronFlash = 0;
  }

  // digital glitch: short blocks that flash bright or cut to black
  glitchIn -= dt;
  if (glitchIn <= 0 && glitchLeft <= 0) {
    glitchIn = frand(1.0f, 3.0f);
    glitchLeft = frand(0.05f, 0.12f);
    glitchLen = random(3, 13);
    glitchStart = random(NUM_LEDS - 12);
    glitchKind = random(2);
  }
  if (glitchLeft > 0) {
    glitchLeft -= dt;
    for (int n = glitchStart; n < glitchStart + glitchLen && n < NUM_LEDS; n++) {
      if (glitchKind == 0) { buf[n][0] = 1.0f; buf[n][1] = G; buf[n][2] = 0.02f; }
      else                 { buf[n][0] = 0; buf[n][1] = 0; buf[n][2] = 0; }
    }
  }

  // cap overlaps by scaling so the red never washes out to pink or white
  for (int n = 0; n < NUM_LEDS; n++) {
    float m = fmaxf(buf[n][0], fmaxf(buf[n][1], buf[n][2]));
    if (m > 1.0f) { buf[n][0] /= m; buf[n][1] /= m; buf[n][2] /= m; }
  }
}

// ---------- Effect 20: Screen sync (Mac ambilight) ----------
// The Mac script sends one UDP packet per frame: [0xA5, count, R,G,B, R,G,B, ...].
// The Mac already did the mapping (which LED shows which part of the screen),
// so this effect just shows the colors it receives, smoothed a little.
WiFiUDP udp;
bool udpStarted = false;
uint8_t udpTarget[NUM_LEDS][3];
float udpCur[NUM_LEDS][3];
uint32_t lastUdp = 0;
bool syncSeen = false;

void startUdp() {
  if (udpStarted) return;
  udp.begin(SYNC_PORT);
  udpStarted = true;
}

void udpTick() {                                // drain every waiting packet
  if (!udpStarted) return;
  int sz = udp.parsePacket();
  while (sz > 0) {
    uint8_t pkt[512];
    int len = udp.read(pkt, sizeof(pkt));
    if (sz <= (int)sizeof(pkt) && len >= 2 && pkt[0] == 0xA5) {
      int count = pkt[1];
      if (count > NUM_LEDS) count = NUM_LEDS;
      if (len >= 2 + count * 3) {
        memset(udpTarget, 0, sizeof(udpTarget));
        memcpy(udpTarget, pkt + 2, count * 3);
        lastUdp = millis();
        syncSeen = true;
      }
    }
    sz = udp.parsePacket();
  }
}

// ---- USB serial input for screen sync (lower, steadier delay than Wi-Fi) ----
// Frame: 0xA5 0x5A, count, count*3 bytes of R,G,B, then a checksum (XOR of count and all color bytes).
// A frame with count = 0 means "the Mac script stopped": the strip goes back to the chill playlist.
uint8_t serBuf[NUM_LEDS * 3];
// Command frame (used by the Mac menu bar app's "Extras" menu): 0xA5 0x5A 0xFE, cmd, arg, checksum (XOR of 0xFE, cmd, arg).
//   cmd 1 = play effect number arg (0..19)     cmd 2 = start playlist arg (1 = Mix, 2 = Chill, 3 = Crazy)
uint8_t serState = 0, serCount = 0, serXor = 0, serCmd = 0, serArg = 0;
int serPos = 0;
uint32_t lastSerialByte = 0, lastSerial = 0;
bool serialWake = false, serialRelease = false;
bool cmdPending = false;
uint8_t pendingCmd = 0, pendingArg = 0;

void serialTick() {
  uint32_t now = millis();
  if (serState != 0 && (now - lastSerialByte) > 100) serState = 0;      // a frame stalled half way: start over
  while (Serial.available() > 0) {
    uint8_t b = (uint8_t)Serial.read();
    lastSerialByte = millis();
    switch (serState) {
      case 0: if (b == 0xA5) serState = 1; break;
      case 1: serState = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); break;
      case 2:
        if (b == 0xFE) { serXor = b; serState = 5; break; }                // command frame
        serCount = b; serXor = b; serPos = 0;
        if (serCount > NUM_LEDS) serState = 0;
        else serState = (serCount == 0) ? 4 : 3;
        break;
      case 3:
        serBuf[serPos++] = b; serXor ^= b;
        if (serPos >= serCount * 3) serState = 4;
        break;
      case 4:
        if (b == serXor) {                                              // good frame
          if (serCount == 0) serialRelease = true;
          else {
            memset(udpTarget, 0, sizeof(udpTarget));
            memcpy(udpTarget, serBuf, serCount * 3);
            uint32_t t = millis();
            if (lastSerial == 0 || (t - lastSerial) > 1500) serialWake = true;   // first frame after a pause
            lastSerial = t;
            lastUdp = t;
            syncSeen = true;
          }
        }
        serState = 0;
        break;
      case 5: serCmd = b; serXor ^= b; serState = 6; break;
      case 6: serArg = b; serXor ^= b; serState = 7; break;
      case 7:
        if (b == serXor) { pendingCmd = serCmd; pendingArg = serArg; cmdPending = true; }
        serState = 0;
        break;
    }
  }
}

void screenSync(float dt) {
  if (!syncSeen || (millis() - lastUdp) > 3000) {          // no data from the Mac: dim waiting pulse
    float p = 0.5f + 0.5f * sinf(millis() / 1000.0f * 2.0f);
    memset(udpCur, 0, sizeof(udpCur));
    for (int n = 0; n < NUM_LEDS; n++) { buf[n][0] = 0.01f + 0.02f * p; buf[n][1] = 0.01f + 0.02f * p; buf[n][2] = 0.03f + 0.05f * p; }
    return;
  }
  for (int n = 0; n < NUM_LEDS; n++)
    for (int c = 0; c < 3; c++) {
      float d = udpTarget[n][c] - udpCur[n][c];
      float rate = SYNC_SMOOTH + SYNC_SNAP * fabsf(d) / 255.0f;   // small flicker is softened, big changes are fast
      udpCur[n][c] += d * fminf(1.0f, dt * rate);
      buf[n][c] = udpCur[n][c] / 255.0f;
    }
}

// =====================================================
//  WEB PAGE (served from the ESP32)
// =====================================================
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>LED Strip</title>
<style>
body{margin:0 auto;font-family:system-ui,sans-serif;background:#0b0b14;color:#eee;padding:14px;max-width:560px}
h1{font-size:20px;margin:6px 0 4px}
h2{font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:#8a8aa8;margin:20px 0 8px}
.row{display:flex;gap:10px;align-items:center;margin:8px 0}
.row label{width:84px;font-size:14px;color:#aab}
input[type=range]{flex:1}
input[type=color]{width:56px;height:36px;border:0;background:none;padding:0}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
button{background:#1c1c30;color:#eee;border:1px solid #2c2c4a;border-radius:10px;padding:12px 8px;font-size:15px}
button.on{background:#5b3df5;border-color:#8b78ff}
#pwr{width:100%}
#pad{height:96px;border-radius:12px;background:linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00);touch-action:none;opacity:.85}
.net{display:flex;justify-content:space-between;padding:6px 2px;border-bottom:1px solid #22223a;font-size:14px}
.dim{color:#8a8aa8;font-size:13px}
</style></head><body>
<h1>LED Strip</h1>
<button id="pwr" onclick="togglePwr()">Power</button>

<h2>Controls</h2>
<div class="row"><label>Brightness</label><input id="bri" type="range" min="5" max="100" value="50"></div>
<div class="row"><label>Speed</label><input id="spd" type="range" min="20" max="300" value="100"></div>
<div class="row"><label>Color</label><input id="col" type="color" value="#ff5000"><span class="dim">for Solid and Paint</span></div>

<h2>Auto playlists</h2><div class="grid" id="autos"></div>
<h2>Relaxing</h2><div class="grid" id="chill"></div>
<h2>Crazy</h2><div class="grid" id="crazy"></div>
<h2>Wi-Fi and color</h2><div class="grid" id="wifi"></div>

<h2>Paint on the strip</h2>
<div id="pad"></div>
<div class="row"><label><input type="checkbox" id="rb" checked> Rainbow</label>
<label><input type="checkbox" id="fd"> Fade</label>
<button onclick="api('/clear')" style="margin-left:auto">Clear</button></div>

<h2>Nearby networks</h2>
<button onclick="scan()" style="width:100%">Scan networks</button>
<div id="nets" style="margin-top:8px"></div>

<div class="dim" id="info" style="margin-top:28px"></div>
<script>
const $=id=>document.getElementById(id);
const api=p=>fetch(p).then(r=>r.text()).catch(()=>{});
const lists={
 autos:[["a1","Mix","auto=1"],["a2","Chill","auto=2"],["a3","Crazy","auto=3"]],
 chill:[[7,"Breathing"],[8,"Sunset"],[9,"Raindrops"],[10,"Candlelight"],[11,"Fireflies"],[12,"Starry night"],[13,"Pastel drift"],[0,"Aurora"],[1,"Lava lamp"],[4,"Ocean"]],
 crazy:[[2,"Fire"],[3,"Hyperspace"],[5,"Lightning"],[6,"Kaleidoscope"],[17,"Tron Legacy (blue)"],[18,"Tron: Clu (orange)"],[19,"Tron: Ares (red)"]],
 wifi:[[14,"Wi-Fi radar"],[16,"Solid color"],[15,"Paint mode"],[20,"Screen sync (Mac)"]]
};
for(const g in lists){for(const it of lists[g]){
 const b=document.createElement("button");
 const isAuto=g==="autos";
 b.textContent=it[1];b.dataset.k=isAuto?it[0]:"m"+it[0];
 b.onclick=()=>{api("/set?"+(isAuto?it[2]:"mode="+it[0]));setTimeout(state,150)};
 $(g).appendChild(b);
}}
let pwr=1;
function togglePwr(){pwr=pwr?0:1;api("/set?pwr="+pwr);$("pwr").textContent=pwr?"Power: ON":"Power: OFF"}
function throttle(fn,ms){let l=0;return(...a)=>{const n=Date.now();if(n-l>=ms){l=n;fn(...a)}}}
$("bri").oninput=throttle(e=>api("/set?bri="+(e.target.value/100)),80);
$("bri").onchange=e=>api("/set?bri="+(e.target.value/100));
$("spd").oninput=throttle(e=>api("/set?spd="+(e.target.value/100)),80);
$("spd").onchange=e=>api("/set?spd="+(e.target.value/100));
$("col").oninput=throttle(e=>{api("/color?hex="+e.target.value.substring(1));setTimeout(state,200)},100);
$("fd").onchange=e=>api("/fade?on="+(e.target.checked?1:0));
const pad=$("pad");
function paint(e){
 const r=pad.getBoundingClientRect();
 const x=Math.min(1,Math.max(0,(e.clientX-r.left)/r.width));
 api("/paint?x="+x.toFixed(3)+"&rb="+($("rb").checked?1:0));
}
const tp=throttle(paint,30);
pad.addEventListener("pointerdown",e=>{pad.setPointerCapture(e.pointerId);paint(e)});
pad.addEventListener("pointermove",e=>{if(e.buttons)tp(e)});
async function state(){
 try{
  const s=await(await fetch("/state")).json();
  document.querySelectorAll("button[data-k]").forEach(b=>b.classList.remove("on"));
  const key=s.auto?"a"+(s.list+1):"m"+s.cur;
  const el=document.querySelector('button[data-k="'+key+'"]');if(el)el.classList.add("on");
  if(s.auto){const c=document.querySelector('button[data-k="m'+s.cur+'"]');if(c)c.style.outline="1px dashed #8b78ff"}
  document.querySelectorAll("button[data-k]").forEach(b=>{if(!s.auto||b.dataset.k!=="m"+s.cur)b.style.outline=""});
  pwr=s.pwr;$("pwr").textContent=pwr?"Power: ON":"Power: OFF";
  $("info").innerHTML="Address: "+s.ip+" | ledstrip.local | <a href=\"/setup\" style=\"color:#8b78ff\">Wi-Fi setup</a>";
 }catch(e){}
}
async function scan(){
 $("nets").innerHTML='<span class="dim">Scanning... (the page may pause for a few seconds)</span>';
 await api("/scan");
 for(let i=0;i<8;i++){
  await new Promise(r=>setTimeout(r,1500));
  try{
   const d=await(await fetch("/nets")).json();
   if(d.fresh){
    $("nets").innerHTML=d.nets.length?d.nets.map(n=>'<div class="net"><span>'+(n.sec?"":"[open] ")+n.ssid+'</span><span class="dim">ch '+n.ch+' | '+n.rssi+' dBm</span></div>').join(""):'<span class="dim">No networks found</span>';
    return;
   }
  }catch(e){}
 }
 $("nets").innerHTML='<span class="dim">Scan timed out, try again</span>';
}
state();setInterval(state,4000);
</script></body></html>
)rawliteral";

// =====================================================
//  STATE + WEB HANDLERS
// =====================================================
WebServer server(80);

const uint8_t PL_ALL[]    = {7, 0, 8, 4, 9, 1, 10, 2, 11, 3, 17, 12, 6, 18, 13, 19, 5};
const uint8_t PL_RELAX[]  = {7, 8, 0, 9, 10, 4, 11, 12, 1, 13};
const uint8_t PL_ENERGY[] = {2, 3, 17, 18, 19, 5, 6};

const uint8_t *playlist;
int listLen = 0, listIdx = 0, listMode = 0;

uint32_t effectStart = 0;
bool autoPlay = true;
bool powerOn = true;
float pwrLevel = 1.0f;
float brightness = START_BRIGHTNESS;
float speedMul = 1.0f;
float tS = 0;                    // scaled animation time (speed slider changes how fast it runs)

bool isRelaxing(int e) { return (e >= 7 && e < 17) || e == 0 || e == 1 || e == 4; }
float durFor(int e)  { return isRelaxing(e) ? RELAX_SECONDS : EFFECT_SECONDS; }
float fadeFor(int e) { return (e == 15 || e == 16 || e == 20) ? 0.25f : (isRelaxing(e) ? 2.5f : 1.0f); }

void setPlaylist(int m) {
  listMode = m;
  if (m == 1)      { playlist = PL_RELAX;  listLen = sizeof(PL_RELAX); }
  else if (m == 2) { playlist = PL_ENERGY; listLen = sizeof(PL_ENERGY); }
  else             { playlist = PL_ALL;    listLen = sizeof(PL_ALL); }
}

void startEffect(int e, uint32_t now) {
  effect = e;
  effectStart = now;
  clearAll();
  memset(heat, 0, sizeof(heat));
  memset(bolt, 0, sizeof(bolt));
  memset(stars, 0, sizeof(stars));
  memset(drops, 0, sizeof(drops));
  glow = 0; flashesLeft = 0; meteorOn = false;
  if (e == 11) initFlies();
  if (e == 18) initClu();
  else if (e == 19) initAres();
  else if (e == 17) initTron(false);
  if (e == 14) { lastScan = now - 7000; sweepPrev = 0; memset(blip, 0, sizeof(blip)); }
}

// ---------- Wi-Fi connection management ----------
Preferences prefs;
String wifiSsid, wifiPass, webPass;
bool apMode = false, netStarted = false;
uint32_t wifiTryStart = 0, lastWifiCheck = 0, apStart = 0, connectedAt = 0;

void startFallbackAP() {
  apMode = true;
  apStart = millis();
  WiFi.mode(WIFI_AP_STA);                       // keep station mode so it can still keep trying
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS);
  startUdp();
  Serial.println("Could not join Wi-Fi. Setup hotspot: " FALLBACK_AP_SSID);
}

void netTick(uint32_t now) {
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !netStarted) {                      // just connected
    netStarted = true;
    startUdp();
    connectedAt = now;
    MDNS.begin(HOSTNAME);
    ArduinoOTA.setHostname(HOSTNAME);
    if (strlen(OTA_PASS)) ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() { Serial.println("OTA update starting"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      // progress bar on the strip: green fills from the start
      int lit = (int)((uint64_t)done * NUM_LEDS / total);
      int i = 0;
      for (int n = 0; n < NUM_LEDS; n++) {
        bool on = n < lit;
        i = putByte(i, on ? 40 : 0);   // Green
        i = putByte(i, 0);             // Red
        i = putByte(i, on ? 6 : 2);    // Blue
      }
      rmtWrite(LED_PIN, ledData, i, RMT_WAIT_FOR_EVER);
    });
    ArduinoOTA.onError([](ota_error_t err) {
      // update failed: whole strip dim red
      int i = 0;
      for (int n = 0; n < NUM_LEDS; n++) { i = putByte(i, 0); i = putByte(i, 40); i = putByte(i, 0); }
      rmtWrite(LED_PIN, ledData, i, RMT_WAIT_FOR_EVER);
    });
    ArduinoOTA.begin();
    Serial.print("Connected. Open http://");
    Serial.println(WiFi.localIP());
  }
  if (!up && !netStarted && !apMode && (now - wifiTryStart) > 20000) startFallbackAP();
  if (!up && netStarted && (now - lastWifiCheck) > 15000) {   // connection dropped
    lastWifiCheck = now;
    WiFi.reconnect();
  }
}

// Status animations drawn on the strip (return true if one is showing)
bool statusOverlay(uint32_t now) {
  float tt = now / 1000.0f;
  if (!netStarted && !apMode) {                 // connecting: orange dot bounces
    clearAll();
    addBlob((0.5f + 0.5f * sinf(tt * 2.5f)) * (NUM_LEDS - 1), 2.0f, 1.0f, 0.45f, 0.0f, 0.9f);
    return true;
  }
  if (apMode && !netStarted && (now - apStart) < 8000) {      // setup hotspot active: blue pulse
    float p = 0.5f + 0.5f * sinf(tt * 3.0f);
    for (int n = 0; n < NUM_LEDS; n++) { buf[n][0] = 0; buf[n][1] = 0.1f * p; buf[n][2] = 0.5f * p; }
    return true;
  }
  if (netStarted && (now - connectedAt) < 1500) {             // connected: green fades out
    float k = 1.0f - (now - connectedAt) / 1500.0f;
    for (int n = 0; n < NUM_LEDS; n++) { buf[n][0] = 0; buf[n][1] = 0.8f * k; buf[n][2] = 0.15f * k; }
    return true;
  }
  return false;
}

// Wrap a handler so it asks for the page password (if WEB_PASS is set)
void route(const char *path, void (*fn)()) {
  server.on(path, [fn]() {
    bool setupHotspot = (apMode && !netStarted);   // hotspot already needs its own password
    if (webPass.length() && !setupHotspot && !server.authenticate("admin", webPass.c_str())) {
      server.requestAuthentication();
      return;
    }
    fn();
  });
}

String jsonEscape(const String &s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 32) o += c;
  }
  return o;
}

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void handleSet() {
  if (server.hasArg("pwr")) powerOn = server.arg("pwr").toInt() != 0;
  if (server.hasArg("bri")) brightness = constrain(server.arg("bri").toFloat(), 0.03f, 1.0f);
  if (server.hasArg("spd")) speedMul = constrain(server.arg("spd").toFloat(), 0.2f, 3.0f);
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m <= 20) { autoPlay = false; startEffect(m, millis()); }
  }
  if (server.hasArg("auto")) {
    int a = constrain(server.arg("auto").toInt(), 1, 3);
    setPlaylist(a - 1);
    autoPlay = true;
    listIdx = 0;
    startEffect(playlist[0], millis());
  }
  server.send(200, "text/plain", "ok");
}

void handleColor() {
  if (server.hasArg("hex")) {
    long v = strtol(server.arg("hex").c_str(), NULL, 16);
    colR = (v >> 16) & 255; colG = (v >> 8) & 255; colB = v & 255;
  }
  if (effect != 15 && effect != 16) { autoPlay = false; startEffect(16, millis()); }
  server.send(200, "text/plain", "ok");
}

void handlePaint() {
  float x = constrain(server.arg("x").toFloat(), 0.0f, 1.0f);
  float w = server.hasArg("w") ? constrain(server.arg("w").toFloat(), 0.8f, 8.0f) : 2.5f;
  if (effect != 15) { autoPlay = false; startEffect(15, millis()); }
  float r = colR / 255.0f, g = colG / 255.0f, b = colB / 255.0f;
  if (server.arg("rb") == "1") hueToRgb(x * 1.5f + millis() / 3000.0f, r, g, b);
  float p = x * (NUM_LEDS - 1);
  int lo = max(0, (int)(p - 3 * w)), hi = min(NUM_LEDS - 1, (int)(p + 3 * w) + 1);
  for (int n = lo; n <= hi; n++) {
    float d = n - p;
    float v = expf(-d * d / (2 * w * w));
    paintBuf[n][0] = paintBuf[n][0] * (1 - v) + r * v;
    paintBuf[n][1] = paintBuf[n][1] * (1 - v) + g * v;
    paintBuf[n][2] = paintBuf[n][2] * (1 - v) + b * v;
  }
  server.send(200, "text/plain", "ok");
}

void handleClear() { memset(paintBuf, 0, sizeof(paintBuf)); server.send(200, "text/plain", "ok"); }

void handleFade() { paintFade = server.arg("on").toInt() != 0; server.send(200, "text/plain", "ok"); }

void handleScan() { scanFresh = false; wantScan = true; server.send(200, "text/plain", "ok"); }

void handleNets() {
  String s = "{\"fresh\":";
  s += scanFresh ? 1 : 0;
  s += ",\"nets\":[";
  for (int i = 0; i < netCount; i++) {
    if (i) s += ",";
    s += "{\"ssid\":\"" + (nets[i].ssid.length() ? jsonEscape(nets[i].ssid) : String("(hidden)")) + "\"";
    s += ",\"rssi\":" + String(nets[i].rssi);
    s += ",\"ch\":" + String(nets[i].ch);
    s += ",\"sec\":" + String(nets[i].secured ? 1 : 0) + "}";
  }
  s += "]}";
  server.send(200, "application/json", s);
}

void handleSetup() {
  String h = "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Setup</title></head>"
             "<body style='font-family:system-ui;background:#0b0b14;color:#eee;padding:16px;max-width:420px;margin:auto'>"
             "<h2>Setup</h2><form method=post action=/savewifi>"
             "<p><b>Home Wi-Fi</b> (2.4 GHz). Leave blank to keep the current one. The strip restarts if you change it.</p>"
             "<p><input name=ssid placeholder='Wi-Fi name' style='width:100%;padding:10px'></p>"
             "<p><input name=pass type=password placeholder='Wi-Fi password' style='width:100%;padding:10px'></p>"
             "<p><b>Page password</b> (user name: admin). Leave blank to keep the current one.</p>"
             "<p><input name=webpass type=password placeholder='New page password' style='width:100%;padding:10px'></p>"
             "<p><label><input type=checkbox name=rmpass value=1> Remove the page password</label></p>"
             "<p><button style='padding:10px 18px'>Save</button></p></form>"
             "<p><a href=/ style='color:#8b78ff'>Back</a></p></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handleSaveWifi() {
  String ssid = server.arg("ssid");
  String wp = server.arg("webpass");
  bool rm = server.arg("rmpass") == "1";
  if (ssid.length() == 0 && wp.length() == 0 && !rm) { server.send(400, "text/plain", "Nothing to save"); return; }

  if (rm) { webPass = ""; prefs.putString("web", ""); }
  else if (wp.length() > 0) { webPass = wp; prefs.putString("web", wp); }

  if (ssid.length() > 0) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", server.arg("pass"));
    server.send(200, "text/plain", "Saved. Restarting. Then open http://" HOSTNAME ".local on your home Wi-Fi.");
    delay(800);
    ESP.restart();
  } else {
    server.send(200, "text/plain", "Saved. Go back to the page (it may ask for the new password).");
  }
}

void handleState() {
  char hex[8];
  snprintf(hex, sizeof(hex), "%02x%02x%02x", colR, colG, colB);
  String s = "{\"cur\":" + String(effect);
  s += ",\"auto\":" + String(autoPlay ? 1 : 0);
  s += ",\"list\":" + String(listMode);
  s += ",\"pwr\":" + String(powerOn ? 1 : 0);
  s += ",\"bri\":" + String(brightness, 2);
  s += ",\"spd\":" + String(speedMul, 2);
  s += ",\"nets\":" + String(netCount);
  s += ",\"color\":\"" + String(hex) + "\"";
  String ip = (apMode && !netStarted) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  s += ",\"ip\":\"" + ip + "\",\"ap\":" + String(apMode ? 1 : 0) + "}";
  server.send(200, "application/json", s);
}

// =====================================================
//  Main
// =====================================================
void setup() {
  Serial.setRxBufferSize(2048);                  // must be set before begin(): holds a few frames while the strip is being updated
  Serial.begin(SYNC_BAUD);
  rmtInit(LED_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  randomSeed(esp_random());
  for (int n = 0; n < 256; n++)
    gammaTable[n] = (uint8_t)(powf(n / 255.0f, 2.2f) * 255.0f + 0.5f);
  memset(paintBuf, 0, sizeof(paintBuf));

  prefs.begin("wifi", false);                    // Wi-Fi details saved from the setup page win over the defaults
  wifiSsid = prefs.getString("ssid", HOME_SSID);
  wifiPass = prefs.getString("pass", HOME_PASS);
  webPass  = prefs.getString("web", WEB_PASS);    // page password (saved value wins over the default)
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                          // faster page response
  wifiTryStart = millis();
  if (wifiSsid.length() && wifiSsid != "YourWiFiName") WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  else startFallbackAP();

  route("/", handleRoot);
  route("/set", handleSet);
  route("/color", handleColor);
  route("/paint", handlePaint);
  route("/clear", handleClear);
  route("/fade", handleFade);
  route("/scan", handleScan);
  route("/nets", handleNets);
  route("/state", handleState);
  route("/setup", handleSetup);
  route("/savewifi", handleSaveWifi);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();

  setPlaylist(PLAY_MODE);
  startEffect(playlist[0], millis());
}

void loop() {
  server.handleClient();                        // answer the phone
  udpTick();                                    // screen-sync packets from the Mac (Wi-Fi)
  serialTick();                                 // screen-sync frames from the Mac (USB)
  if (serialWake) {                             // USB frames started arriving: switch to Screen sync
    serialWake = false;
    if (effect != 20) { autoPlay = false; startEffect(20, millis()); }
  }
  if (serialRelease) {                          // Mac script stopped: back to the chill playlist
    serialRelease = false;
    if (effect == 20) { setPlaylist(1); autoPlay = true; listIdx = 0; startEffect(playlist[0], millis()); }
  }
  if (cmdPending) {                             // pattern chosen from the Mac app (the app repeats it a few times, so repeats are ignored)
    cmdPending = false;
    lastSerial = 0;                             // the next screen frame will switch back to Screen sync
    if (pendingCmd == 1 && pendingArg <= 19) {
      if (!(effect == pendingArg && !autoPlay)) { autoPlay = false; startEffect(pendingArg, millis()); }
    } else if (pendingCmd == 2 && pendingArg >= 1 && pendingArg <= 3) {
      if (!(autoPlay && listMode == pendingArg - 1 && effect != 20)) {
        setPlaylist(pendingArg - 1); autoPlay = true; listIdx = 0; startEffect(playlist[0], millis());
      }
    }
  }
  ArduinoOTA.handle();                          // wireless updates

  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 10) return;                  // about 100 frames per second
  float dt = (now - last) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  last = now;

  netTick(now);
  scanTick(now);

  float dtS = dt * speedMul;
  tS += dtS;
  float t = tS;
  uint32_t nowS = (uint32_t)(tS * 1000.0f);

  pwrLevel += (powerOn ? 1.0f : -1.0f) * dt / 0.8f;   // smooth power on/off
  pwrLevel = clamp01(pwrLevel);

  bool overlay = statusOverlay(now);             // connection status animations
  static bool wasOverlay = false;
  if (wasOverlay && !overlay) effectStart = now;
  wasOverlay = overlay;

  if (!overlay && autoPlay && (now - effectStart) > durFor(effect) * 1000.0f) {
    listIdx = (listIdx + 1) % listLen;
    startEffect(playlist[listIdx], now);
  }

  if (!overlay) switch (effect) {
    case 0:  aurora(t);             break;
    case 1:  lava(t);               break;
    case 2:  fire(dtS);             break;
    case 3:  hyperspace(dtS, t);    break;
    case 4:  ocean(t);              break;
    case 5:  lightning(nowS, dtS);  break;
    case 6:  kaleido(t);            break;
    case 7:  breathing(t);          break;
    case 8:  sunset(t);             break;
    case 9:  raindrops(dtS);        break;
    case 10: candle(t);             break;
    case 11: fireflies(dtS);        break;
    case 12: starry(dtS, t);        break;
    case 13: pastel(t);             break;
    case 14: radar(t, dtS);         break;
    case 15: paintEffect(dt);       break;
    case 16: solidColor();          break;
    case 17: tron(t, dtS, 0, false); break;
    case 18: tronClu(t, dtS);        break;
    case 19: tronAres(t, dtS);       break;
    case 20: screenSync(dt);         break;
  }

  // Smooth fade in/out between effects
  float el = (now - effectStart) / 1000.0f;
  float fs = fadeFor(effect);
  float fade = autoPlay ? clamp01(fminf(el / fs, (durFor(effect) - el) / fs))
                        : clamp01(el / fs);
  fade = fade * fade * (3.0f - 2.0f * fade);
  fade *= pwrLevel;
  if (overlay) fade = 1.0f;

  // Gamma + power total
  static uint8_t out[NUM_LEDS][3];
  uint32_t sum = 0;
  for (int n = 0; n < NUM_LEDS; n++) {
    for (int c = 0; c < 3; c++) {
      float v = clamp01(buf[n][c] * brightness * fade);
      out[n][c] = gammaTable[(uint8_t)(v * 255.0f + 0.5f)];
      sum += out[n][c];
    }
  }

  // Power limiter: each channel draws about 20 mA at 255
  float amps = (sum / 255.0f) * 0.02f;
  float scale = (amps > MAX_AMPS) ? (MAX_AMPS / amps) : 1.0f;

  int i = 0;
  for (int n = 0; n < NUM_LEDS; n++) {
    i = putByte(i, (uint8_t)(out[n][1] * scale));  // Green
    i = putByte(i, (uint8_t)(out[n][0] * scale));  // Red
    i = putByte(i, (uint8_t)(out[n][2] * scale));  // Blue
  }
  rmtWrite(LED_PIN, ledData, i, RMT_WAIT_FOR_EVER);
}

// SPDX-License-Identifier: MIT
//
// Clawrophyll Kids -- offline, sensor-driven spin-off of Clawrophyll.
//
// No AI, no internet, no camera/mic/speaker. Everything runs here on the MCU:
// the sensors ARE the controls, and every poke gets an instant face + arm reaction.
// Same wiring as Clawrophyll (Modulino Thermo/Light/Distance on Qwiic, soil probe
// on A0, GC9A01 round display on hardware SPI, SG90 pincer on D3).
//
// How it behaves:
//   Reactions   a sensor change -> face + arm gesture (2 s, or until the arm finishes) -> readout card of
//               the sensor that caused it (~3 s) -> back to the face, which holds the
//               reaction while the condition lasts.
//                 hand in front (12-40 cm) -> happy + wave
//                 hand very close (<12 cm) -> surprised + slow nudge
//                 hand stays 5 s+          -> hearts
//                 3 jabs (<10 cm) in 10 s  -> angry "Hey!"
//                 light sensor covered     -> sleepy Zzz + slump
//                 torch on light sensor    -> too bright + shades his eyes
//                 breath on thermo sensor  -> sweating + fans himself
//                 sudden cold              -> freezing + shiver
//                 probe in wet soil        -> too wet
//                 probe out in the air     -> thirsty + slump
//   Idle        a calm face with the occasional blink, and a random
//               "skit" every 20 s to 2 min (wave, hearts, dizzy, dance, yawn, sneeze,
//               or a tour of his senses on the display).
//
// Triggers compare against slowly-adapting baselines (not fixed numbers), so a hot
// classroom or a dim room doesn't leave him stuck in one face; only the kids' actions
// set him off. The soil probe and distance sensor use fixed thresholds instead.
//
// Safety: arm_angle() clamps every servo command to [kArmMin, kArmMax] in firmware,
// and gestures near a very close hand are slowed down.
//
// The only Linux-side code is a passive logger (python/main.py) that prints
// read_status() every few seconds for tuning. Nothing here depends on it.

#include <Arduino_RouterBridge.h>
#include <Arduino_Modulino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Servo.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "types.h"

// --- Hardware ----------------------------------------------------------------
const int kSoilPin = A0;
const int kTftDC = 7, kTftCS = 10, kTftRST = 8;
Adafruit_GC9A01A tft(&SPI, kTftDC, kTftCS, kTftRST);
ModulinoThermo thermo; ModulinoLight light; ModulinoDistance distance;
bool thermo_ok = false, light_ok = false, distance_ok = false;

const uint16_t kBlack = 0x0000, kWhite = 0xFFFF, kCyan = 0x07FF, kGreen = 0x07E0;
const uint16_t kAmber = 0xFD20, kOrange = 0xFBE0, kIce = 0xAEFF;
const uint16_t kDimBlu = 0x4A49, kYellow = 0xFFE0, kWater = 0x051F;
const uint16_t kRed = 0xF800, kSoil = 0xCB40, kPink = 0xFA8F, kPurple = 0xB01F;
const uint16_t kLime = 0x87E0, kGray = 0x8410, kRingBg = 0x2104;

// --- Tuning knobs --------------------------------------------------------------
const int kSoilDry = 944, kSoilWet = 594;           // probe raw in air / in water
const unsigned long kSkitMinMs = 20000, kSkitMaxMs = 120000;  // idle skit interval
const unsigned long kReactMs = 2000, kCardMs = 3000;          // reaction face, then card
const unsigned long kRepeatMs = 20000;  // same reaction again within this = face only
const unsigned long kArmCooldownMs = 12000;  // arm rests this long between gestures (faces still react)
const unsigned long kStaleMs = 20000;   // condition held this long -> back to idle
const unsigned long kRebaseMs = 60000;  // ...this long -> accept it as the new normal
const unsigned long kNagMs = 60000;     // stale soil/hand condition: replay this often
const unsigned long kLoveMs = 5000;     // hand held in front this long -> hearts
const unsigned long kWarmupMs = 3000;   // after boot: settle baselines, no reactions yet

// --- Sensors -----------------------------------------------------------------
struct Sensors {
  float temp_c = NAN, hum_pct = NAN, dist_mm = NAN, soil_pct = NAN;
  int lux = -1, soil_raw = -1;
} s;
// Baselines: slow averages of "normal", frozen while a reaction is active.
float base_lux = NAN, base_hum = NAN, base_temp = NAN;

void poll_sensors() {
  if (distance_ok) s.dist_mm = distance.available() ? distance.get() : NAN;  // NAN = no target
  if (thermo_ok) { s.temp_c = thermo.getTemperature(); s.hum_pct = thermo.getHumidity(); }
  if (light_ok)  { light.update(); s.lux = light.getLux(); }
  int raw = analogRead(kSoilPin); s.soil_raw = raw;
  float pct = 100.0f * (float)(kSoilDry - raw) / (float)(kSoilDry - kSoilWet);
  s.soil_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

void ema(float& base, float v, float a) {
  if (isnan(v)) return;
  base = isnan(base) ? v : base + a * (v - base);
}

// --- Faces -------------------------------------------------------------------
const char* kFaceName[] = {"idle","happy","sleepy","angry","thirsty","sweating","freezing",
                           "too_wet","too_bright","surprised","love","dizzy","music",
                           "squint","sneeze"};

const int kEyeW = 68, kEyeH = 84, kEyeR = 26, kEyeCY = 108, kLeftCX = 76, kRightCX = 164;
EyeP eye_params(Face f) {
  switch (f) {
    case F_HAPPY:      return {kGreen,  0.55f, 1.00f, 0};
    case F_SLEEPY:     return {kDimBlu, 0.20f, 1.00f, 8};
    case F_ANGRY:      return {kRed,    0.70f, 1.00f, 4};
    case F_THIRSTY:    return {kAmber,  0.50f, 0.90f, 4};
    case F_SWEATING:   return {kOrange, 1.00f, 1.00f, 0};
    case F_FREEZING:   return {kIce,    0.60f, 0.72f, 0};
    case F_TOO_WET:    return {kCyan,   0.90f, 1.00f, 0};
    case F_TOO_BRIGHT: return {kOrange, 0.30f, 0.95f, 0};
    case F_SQUINT:     return {kCyan,   0.12f, 1.00f, 4};
    default:           return {kCyan,   1.00f, 1.00f, 0};
  }
}
bool blink_ok(Face f) {
  return f == F_IDLE || f == F_HAPPY || f == F_ANGRY || f == F_THIRSTY || f == F_SWEATING;
}

Face face = F_IDLE;      // face on screen (or under the card)
int  card = -1;          // sensor card on screen, -1 = showing the face
bool face_dirty = true;

void thick_line(int x0, int y0, int x1, int y1, int t, uint16_t c) {
  for (int o = -t/2; o <= t/2; o++) {
    tft.drawLine(x0, y0+o, x1, y1+o, c);
    tft.drawLine(x0+o, y0, x1+o, y1, c);
  }
}

void fill_star(int cx, int cy, int R, uint16_t col) {
  int ox[5], oy[5], ix[5], iy[5];
  for (int k = 0; k < 5; k++) {
    float ao = -PI/2 + k * (2*PI/5), ai = ao + PI/5;
    ox[k] = cx + (int)(R * cos(ao));        oy[k] = cy + (int)(R * sin(ao));
    ix[k] = cx + (int)(R * 0.40f * cos(ai)); iy[k] = cy + (int)(R * 0.40f * sin(ai));
  }
  for (int k = 1; k < 4; k++) tft.fillTriangle(ix[0],iy[0], ix[k],iy[k], ix[k+1],iy[k+1], col);
  for (int k = 0; k < 5; k++) tft.fillTriangle(ox[k],oy[k], ix[k],iy[k], ix[(k+4)%5],iy[(k+4)%5], col);
}

// Heart: two circles + a triangle; (cx, cy) is the centre of the lobes.
void fill_heart(int cx, int cy, int r, uint16_t c) {
  tft.fillCircle(cx - r, cy, r, c);
  tft.fillCircle(cx + r, cy, r, c);
  tft.fillTriangle(cx - 2*r + 1, cy + r/3, cx + 2*r - 1, cy + r/3, cx, cy + 2*r + r/3, c);
}

void draw_flake(int x, int y, uint16_t c) {
  tft.fillCircle(x, y, 3, c);
  tft.drawLine(x-6, y, x+6, y, c); tft.drawLine(x, y-6, x, y+6, c);
  tft.drawLine(x-4, y-4, x+4, y+4, c); tft.drawLine(x-4, y+4, x+4, y-4, c);
}

void draw_note(int x, int y, uint16_t c) {   // music note, (x, y) = note head
  tft.fillCircle(x, y, 6, c);
  tft.fillRect(x + 4, y - 22, 3, 22, c);
  tft.fillRect(x + 4, y - 22, 10, 3, c);
}

void text_plain(const char* str, int x, int y, int sz, uint16_t c) {
  tft.setFont(NULL); tft.setTextColor(c); tft.setTextSize(sz);
  tft.setCursor(x, y); tft.print(str);
}

// Centred text in a GFX font; y is the baseline.
void text_center(const char* str, int y, const GFXfont* f, uint16_t c) {
  tft.setFont(f); tft.setTextSize(1); tft.setTextColor(c);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(str, 0, y, &bx, &by, &bw, &bh);
  tft.setCursor(120 - (int)bw/2 - bx, y); tft.print(str);
  tft.setFont(NULL);
}

void overlay_angry() {
  const int yt = 83;
  tft.fillTriangle(110, yt, 110, yt+30, 56, yt, kBlack);
  tft.fillTriangle(130, yt, 130, yt+30, 184, yt, kBlack);
  for (int k = 0; k < 8; k++) {
    tft.drawLine(38, 62+k, 112, 86+k, kRed);
    tft.drawLine(202, 62+k, 128, 86+k, kRed);
  }
}

int dizzy_phase = 0;
void draw_spiral(int cx, int cy, int phase, uint16_t c) {
  float px = cx, py = cy;
  for (int i = 1; i <= 42; i++) {
    float t = i * 0.45f, r = i * 0.8f;
    float a = t + phase * 0.6f;
    float x = cx + r * cos(a), y = cy + r * sin(a);
    thick_line((int)px, (int)py, (int)x, (int)y, 3, c);
    px = x; py = y;
  }
}

// Draws both eyes for the current face. Erases the previous eye box first, so a
// blink or a spinning spiral redraws only the eyes, not the whole screen.
void draw_eyes(bool blinking) {
  const int cxs[2] = {kLeftCX, kRightCX};
  for (int i = 0; i < 2; i++) tft.fillRect(cxs[i] - 42, kEyeCY - 50, 84, 104, kBlack);

  for (int i = 0; i < 2; i++) {
    int cx = cxs[i], cy = kEyeCY;
    if (blinking) { tft.fillRoundRect(cx - 34, cy - 2, 68, 5, 2, eye_params(face).color); continue; }
    switch (face) {
      case F_SURPRISED:
        tft.fillCircle(cx, cy, 34, kWhite);
        tft.fillCircle(cx + (i ? -6 : 6), cy + 4, 13, kBlack);
        break;
      case F_LOVE:
        fill_heart(cx, cy - 14, 17, kPink);
        break;
      case F_DIZZY:
        draw_spiral(cx, cy, dizzy_phase + (i ? 5 : 0), kLime);
        break;
      case F_MUSIC:                                   // closed, smiling "^" eyes
        tft.fillCircle(cx, cy + 6, 32, kPurple);
        tft.fillCircle(cx, cy + 22, 32, kBlack);
        break;
      case F_SNEEZE:                                  // squeezed shut: > <
        if (i == 0) { thick_line(cx-24, cy-22, cx+18, cy, 7, kCyan); thick_line(cx+18, cy, cx-24, cy+22, 7, kCyan); }
        else        { thick_line(cx+24, cy-22, cx-18, cy, 7, kCyan); thick_line(cx-18, cy, cx+24, cy+22, 7, kCyan); }
        break;
      default: {
        EyeP p = eye_params(face);
        int w = (int)(kEyeW * p.w), h = (int)(kEyeH * p.h); if (h < 5) h = 5;
        int r = kEyeR; if (r > w/2) r = w/2; if (r > h/2) r = h/2;
        tft.fillRoundRect(cx - w/2, cy - h/2 + p.yoff, w, h, r, p.color);
      }
    }
  }
  if (face == F_ANGRY && !blinking) overlay_angry();
}

void draw_decor() {
  switch (face) {
    case F_HAPPY:
      fill_star(52,40,18,kYellow); fill_star(120,28,15,kWhite); fill_star(188,40,18,kYellow); break;
    case F_THIRSTY:
      tft.fillRect(0,172,240,68,kSoil);
      thick_line(30,172,70,230,5,kBlack);   thick_line(70,230,120,188,5,kBlack);
      thick_line(120,188,175,232,5,kBlack); thick_line(120,188,205,180,5,kBlack);
      thick_line(95,202,140,216,5,kBlack); break;
    case F_TOO_BRIGHT: {
      tft.fillCircle(240,-15,100,kYellow);
      int ang[4] = {95,120,145,170};
      for (int n = 0; n < 4; n++) { float r = ang[n]*PI/180.0f;
        thick_line(240+(int)(100*cos(r)), -15+(int)(100*sin(r)),
                   240+(int)(124*cos(r)), -15+(int)(124*sin(r)), 3, kYellow); }
      break; }
    case F_FREEZING: text_plain("Brrr", 92, 198, 2, kIce); break;
    case F_ANGRY:    text_center("Hey!", 210, &FreeSansBold12pt7b, kRed); break;
    case F_SURPRISED:
      tft.fillCircle(120, 196, 14, kWhite); tft.fillCircle(120, 196, 8, kBlack);
      text_center("!", 52, &FreeSansBold24pt7b, kYellow); break;
    case F_SQUINT:   text_center("ah... ah...", 205, &FreeSansBold12pt7b, kWhite); break;
    case F_SNEEZE:   text_center("ACHOO!", 210, &FreeSansBold12pt7b, kYellow); break;
    default: break;
  }
}

unsigned long face_start_ms = 0, last_anim_ms = 0;
int sweat_y = 0, water_level = 0, zzz_n = 0, float_y[3] = {0, 0, 0};
int flake_x[3] = {26,120,214}, flake_y[3] = {-10,-50,-90};

void enter_face() {
  tft.fillScreen(kBlack);
  draw_eyes(false);
  draw_decor();
  face_start_ms = millis();
  sweat_y = 66; water_level = 0; zzz_n = 0;
  flake_y[0] = -10; flake_y[1] = -50; flake_y[2] = -90;
  float_y[0] = 200; float_y[1] = 150; float_y[2] = 235;
}

// Per-face frame animation (~11 fps). Things that float use side margins and the
// bottom, outside the eye box.
void animate_face(unsigned long now) {
  if (now - last_anim_ms < 90) return;
  last_anim_ms = now;
  if (face == F_SWEATING) {
    tft.fillRect(206, sweat_y-26, 34, 38, kBlack);
    sweat_y += 12; if (sweat_y > 156) sweat_y = 66;
    tft.fillCircle(218, sweat_y, 10, kWater);
    tft.fillTriangle(208, sweat_y-4, 228, sweat_y-4, 218, sweat_y-24, kWater);
  } else if (face == F_FREEZING) {
    for (int i = 0; i < 3; i++) {
      tft.fillRect(flake_x[i]-8, flake_y[i]-8, 17, 17, kBlack);
      flake_y[i] += 5 + i; if (flake_y[i] > 232) flake_y[i] = -6;
      draw_flake(flake_x[i], flake_y[i], kWhite);
    }
  } else if (face == F_TOO_WET) {
    if (water_level < 150) { water_level += 5; tft.fillRect(0, 240-water_level, 240, 6, kWater); }
    else if (now - face_start_ms > 5000) enter_face();
  } else if (face == F_SLEEPY) {
    if (now - face_start_ms > (unsigned long)(700 + zzz_n*600)) {
      zzz_n = (zzz_n % 3) + 1;
      tft.fillRect(144, 22, 86, 74, kBlack);
      if (zzz_n >= 1) text_plain("z", 150, 78, 2, kIce);
      if (zzz_n >= 2) text_plain("z", 168, 54, 3, kIce);
      if (zzz_n >= 3) text_plain("Z", 182, 30, 4, kIce);
      if (zzz_n == 3) face_start_ms = now;
    }
  } else if (face == F_LOVE || face == F_MUSIC) {
    // three floaters rising: left margin, right margin, bottom centre
    const int fx[3] = {22, 218, 120}, top[3] = {70, 70, 178}, bot[3] = {190, 190, 236};
    for (int i = 0; i < 3; i++) {
      tft.fillRect(fx[i]-14, float_y[i]-26, 30, 44, kBlack);
      float_y[i] -= 4; if (float_y[i] < top[i]) float_y[i] = bot[i];
      if (face == F_LOVE) fill_heart(fx[i], float_y[i], 5, kPink);
      else                draw_note(fx[i]-4, float_y[i], i == 1 ? kYellow : kPurple);
    }
  } else if (face == F_DIZZY) {
    dizzy_phase++;
    draw_eyes(false);
  }
}

// --- Sensor cards --------------------------------------------------------------
// A big live readout with a gauge ring around the round display's edge.
enum Card { C_TEMP, C_HUM, C_LIGHT, C_SOIL, C_DIST, C_COUNT };
const char* kCardLabel[] = {"TEMPERATURE", "HUMIDITY", "LIGHT", "SOIL WATER", "DISTANCE"};
const char* kCardUnit[]  = {"C", "%", "lux", "%", "cm"};
const uint16_t kCardColor[] = {kOrange, kCyan, kYellow, kGreen, kPink};
const int kRingSegs = 36;
int ring_drawn = 0;
char card_val[12] = "", card_cap[20] = "";

void ring_seg(int k, uint16_t c) {        // segment k of the 270-degree ring
  float a0 = (135.0f + k * 270.0f / kRingSegs) * PI / 180.0f;
  float a1 = (135.0f + (k + 1) * 270.0f / kRingSegs - 2.0f) * PI / 180.0f;
  const int ro = 117, ri = 103;
  int x0 = 120 + ro*cos(a0), y0 = 120 + ro*sin(a0), x1 = 120 + ro*cos(a1), y1 = 120 + ro*sin(a1);
  int x2 = 120 + ri*cos(a1), y2 = 120 + ri*sin(a1), x3 = 120 + ri*cos(a0), y3 = 120 + ri*sin(a0);
  tft.fillTriangle(x0,y0, x1,y1, x2,y2, c);
  tft.fillTriangle(x0,y0, x2,y2, x3,y3, c);
}

// Current value, gauge fraction (0..1) and a kid-friendly caption for a card.
void card_reading(int c, char* val, float& frac, const char*& cap) {
  switch (c) {
    case C_TEMP: {
      if (isnan(s.temp_c)) { strcpy(val, "--"); frac = 0; cap = ""; break; }
      int t10 = (int)lroundf(s.temp_c * 10);
      snprintf(val, 12, "%d.%d", t10 / 10, abs(t10 % 10));
      frac = (s.temp_c - 15.0f) / 25.0f;
      cap = s.temp_c < 20 ? "Brrr, chilly!" : (s.temp_c > 32 ? "Phew, hot!" : "Comfy!");
      break; }
    case C_HUM: {
      if (isnan(s.hum_pct)) { strcpy(val, "--"); frac = 0; cap = ""; break; }
      snprintf(val, 12, "%d", (int)lroundf(s.hum_pct));
      frac = s.hum_pct / 100.0f;
      cap = (!isnan(base_hum) && s.hum_pct > base_hum + 6) ? "Your breath!"
          : (s.hum_pct > 80 ? "So steamy!" : (s.hum_pct < 35 ? "So dry!" : "Just right"));
      break; }
    case C_LIGHT:
      snprintf(val, 12, "%d", s.lux < 0 ? 0 : s.lux);
      { float l = s.lux < 0 ? 0 : (float)s.lux; frac = l / (l + 300.0f); }  // ~log feel; no libm (log10f won't link)
      cap = s.lux < 15 ? "So dark!" : (s.lux > 1500 ? "So bright!" : "Nice light");
      break;
    case C_SOIL:
      snprintf(val, 12, "%d", (int)lroundf(s.soil_pct));
      frac = s.soil_pct / 100.0f;
      cap = s.soil_pct < 5 ? "I'm thirsty!" : (s.soil_pct > 70 ? "Too soggy!" : "Yummy soil");
      break;
    case C_DIST:
      if (isnan(s.dist_mm)) { strcpy(val, "--"); frac = 0; cap = "Nobody?"; break; }
      snprintf(val, 12, "%d", (int)lroundf(s.dist_mm / 10.0f));
      frac = 1.0f - s.dist_mm / 500.0f;
      cap = s.dist_mm < 120 ? "Too close!" : (s.dist_mm < 400 ? "Hi there!" : "Peekaboo?");
      break;
  }
  frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
}

void card_refresh(bool full) {
  char val[12]; float frac; const char* cap;
  card_reading(card, val, frac, cap);
  uint16_t col = kCardColor[card];
  if (full) {
    tft.fillScreen(kBlack);
    for (int k = 0; k < kRingSegs; k++) ring_seg(k, kRingBg);
    text_center(kCardLabel[card], 72, &FreeSansBold12pt7b, col);
    ring_drawn = 0; card_val[0] = 0; card_cap[0] = 0;
  }
  int segs = (int)lroundf(frac * kRingSegs);
  while (ring_drawn < segs) ring_seg(ring_drawn++, col);
  while (ring_drawn > segs) ring_seg(--ring_drawn, kRingBg);
  if (strcmp(val, card_val) != 0) {
    tft.fillRect(40, 92, 160, 80, kBlack);
    text_center(val, 140, &FreeSansBold24pt7b, kWhite);
    if (card == C_TEMP && strcmp(val, "--") != 0) {           // draw the degree sign by hand
      text_center("C", 166, &FreeSansBold12pt7b, kGray);
      tft.drawCircle(104, 151, 3, kGray); tft.drawCircle(104, 151, 2, kGray);
    } else {
      text_center(kCardUnit[card], 166, &FreeSansBold12pt7b, kGray);
    }
    strcpy(card_val, val);
  }
  if (strcmp(cap, card_cap) != 0) {
    tft.fillRect(40, 176, 160, 34, kBlack);
    text_center(cap, 200, &FreeSansBold12pt7b, kWhite);
    strncpy(card_cap, cap, sizeof(card_cap) - 1);
  }
}

void show_face(Face f) {
  if (card >= 0 || f != face) { face = f; card = -1; face_dirty = true; }
}
void show_card(int c) {
  if (c != card) { card = c; card_refresh(true); }
}

// --- Servo arm (D3) ----------------------------------------------------------
// Non-blocking gesture engine: each gesture is a list of waypoints. Each move is
// TIME-based with cosine easing (accelerate, then decelerate into the waypoint),
// written in microseconds for sub-degree steps. Being time-based, a slow screen
// redraw can't stretch or stall a move; the arm just resumes on its curve.
// step_deg is the average speed in degrees per 20 ms (as in Clawrophyll).
// Attached only while moving, then limp and silent.
const bool kArmMirror = false;           // true = mirror every angle about kArmRest
const int kServoPin = 3;
Servo arm;
const int kArmRest = 90;
const int kArmMin = 20, kArmMax = 160;   // safe clamp, enforced in firmware
const float kArmSpeed = 0.2f;            // global speed scale on every gesture (1 = Clawrophyll's speed)

const Waypoint kWpHome[]    = {{kArmRest,120}};
const Waypoint kWpWave[]    = {{150,60},{110,0},{150,50},{110,0},{150,50},{kArmRest,120}};
const Waypoint kWpNudge[]   = {{55,50},{108,40},{55,50},{108,40},{kArmRest,110}};
const Waypoint kWpSlump[]   = {{105,100},{125,130},{138,160},{142,280}};
const Waypoint kWpDance[]   = {{65,80},{115,80},{65,80},{115,80},{65,80},{115,80},{kArmRest,100}};
const Waypoint kWpWobble[]  = {{78,0},{102,0},{78,0},{102,0},{78,0},{102,0},{kArmRest,60}};
const Waypoint kWpShade[]   = {{150,2200},{kArmRest,100}};
const Waypoint kWpStretch[] = {{155,900},{kArmRest,100}};
const Waypoint kWpSway[]    = {{130,150},{105,150},{130,150},{105,150},{kArmRest,100}};
const Waypoint kWpFan[]     = {{140,0},{115,0},{140,0},{115,0},{140,0},{115,0},{kArmRest,80}};
const Waypoint kWpShiver[]  = {{85,0},{95,0},{85,0},{95,0},{85,0},{95,0},{85,0},{95,0},{kArmRest,0}};
const Waypoint kWpPresent[] = {{150,1200},{kArmRest,100}};

#define GEST(n, w, sp) {n, w, (int)(sizeof(w)/sizeof(w[0])), sp}
enum GestId { G_NONE = -1, G_HOME, G_WAVE, G_NUDGE, G_NUDGE_SOFT, G_SLUMP, G_DANCE, G_WOBBLE,
              G_SHADE, G_STRETCH, G_SWAY, G_FAN, G_SHIVER, G_PRESENT };
const Gesture kGestures[] = {
  GEST("home", kWpHome, 8),       GEST("wave", kWpWave, 9),      GEST("nudge", kWpNudge, 10),
  GEST("nudge_soft", kWpNudge, 5), GEST("slump", kWpSlump, 6),   GEST("dance", kWpDance, 6),
  GEST("wobble", kWpWobble, 7),   GEST("shade", kWpShade, 7),    GEST("stretch", kWpStretch, 3),
  GEST("sway", kWpSway, 4),       GEST("fan", kWpFan, 11),       GEST("shiver", kWpShiver, 4),
  GEST("present", kWpPresent, 8),
};

const Gesture* arm_g = nullptr;
int   arm_step = 0;
float arm_pos = kArmRest;                // current (eased) angle
float seg_from = kArmRest, seg_to = kArmRest;
unsigned long seg_t0 = 0, seg_ms = 0, hold_until = 0;
bool  arm_attached = false, arm_holding = false;
const char* arm_last = "home";
unsigned long arm_idle_since = 0;        // when the last gesture ended (0 = never moved)

int arm_angle(int a) {
  if (kArmMirror) a = 2 * kArmRest - a;
  return a < kArmMin ? kArmMin : (a > kArmMax ? kArmMax : a);
}

void arm_write(float a) {
  arm.writeMicroseconds(MIN_PULSE_WIDTH + (int)lroundf(a * (MAX_PULSE_WIDTH - MIN_PULSE_WIDTH) / 180.0f));
}

unsigned long move_ms(const Gesture* g, float from, float to) {
  float deg_per_ms = g->step_deg / 20.0f * kArmSpeed;
  unsigned long ms = (unsigned long)(fabsf(to - from) / deg_per_ms * 1.1f);
  return ms < 60 ? 60 : ms;
}

// How long a whole gesture takes from the arm's current position (moves + holds).
unsigned long gesture_ms(int gi) {
  if (gi < 0) return 0;
  const Gesture* g = &kGestures[gi];
  float from = arm_pos; unsigned long total = 0;
  for (int i = 0; i < g->len; i++) {
    float to = arm_angle(g->wp[i].angle);
    total += move_ms(g, from, to) + g->wp[i].hold_ms;
    from = to;
  }
  return total;
}

void start_segment(unsigned long now) {
  seg_from = arm_pos;
  seg_to = arm_angle(arm_g->wp[arm_step].angle);
  seg_ms = move_ms(arm_g, seg_from, seg_to);
  seg_t0 = now; arm_holding = false;
}

void service_arm(unsigned long now) {
  if (arm_g == nullptr) return;
  if (!arm_attached) {
    arm_write(arm_pos);                  // set the pulse before the pin goes live
    arm.attach(kServoPin); arm_attached = true;
    arm_write(arm_pos);
    start_segment(now);
    return;
  }
  if (!arm_holding) {
    float u = (float)(now - seg_t0) / (float)seg_ms;
    if (u > 1) u = 1;
    arm_pos = seg_from + (seg_to - seg_from) * (1.0f - cosf(PI * u)) * 0.5f;
    arm_write(arm_pos);
    if (u >= 1) { arm_holding = true; hold_until = now + arm_g->wp[arm_step].hold_ms; }
    return;
  }
  if ((long)(now - hold_until) < 0) return;
  if (++arm_step >= arm_g->len) { arm.detach(); arm_attached = false; arm_g = nullptr; arm_idle_since = now; return; }
  start_segment(now);
}

void play_gesture(int g) {
  if (g < 0) return;
  arm_g = &kGestures[g]; arm_last = arm_g->name;
  arm_step = 0;
  if (arm_attached) start_segment(millis());   // retarget smoothly from where it is
}

// --- Sequences (reactions and idle skits) ---------------------------------------
const Step kSkitHello[]  = {{F_HAPPY, G_WAVE, -1, 2600}};
const Step kSkitLove[]   = {{F_LOVE, G_SWAY, -1, 4200}};
const Step kSkitDizzy[]  = {{F_DIZZY, G_WOBBLE, -1, 3200}};
const Step kSkitDance[]  = {{F_MUSIC, G_DANCE, -1, 4500}};
const Step kSkitYawn[]   = {{F_SLEEPY, G_STRETCH, -1, 3800}, {F_SURPRISED, G_NUDGE, -1, 1500}};
const Step kSkitSneeze[] = {{F_SQUINT, G_NONE, -1, 1500}, {F_SNEEZE, G_NUDGE, -1, 1500}};
const Step kSkitSenses[] = {{F_HAPPY, G_PRESENT, -1, 1400}, {F_HAPPY, G_NONE, C_TEMP, 2500},
                            {F_HAPPY, G_NONE, C_HUM, 2500}, {F_HAPPY, G_NONE, C_LIGHT, 2500},
                            {F_HAPPY, G_NONE, C_SOIL, 2500}, {F_HAPPY, G_NONE, C_DIST, 2500}};
struct Skit { const char* name; const Step* steps; int len; };
#define SK(n, a) {n, a, (int)(sizeof(a)/sizeof(a[0]))}
const Skit kSkits[] = { SK("hello", kSkitHello), SK("love", kSkitLove), SK("dizzy", kSkitDizzy),
                        SK("dance", kSkitDance), SK("yawn", kSkitYawn), SK("sneeze", kSkitSneeze),
                        SK("senses", kSkitSenses) };
const int kSkitCount = sizeof(kSkits) / sizeof(kSkits[0]);

Step react_buf[2];                   // built on the fly for reactions
const Step* seq = nullptr;
int seq_len = 0, seq_i = 0;
unsigned long step_end_ms = 0;
const char* seq_name = "";

void start_step(unsigned long now) {
  const Step& st = seq[seq_i];
  if (st.card >= 0) show_card(st.card); else show_face((Face)st.face);
  // The arm joins in only if it's already moving or has rested kArmCooldownMs.
  bool arm_ok = arm_g != nullptr || arm_idle_since == 0 || now - arm_idle_since >= kArmCooldownMs;
  int g = arm_ok ? st.gest : G_NONE;
  unsigned long g_ms = gesture_ms(g);             // a step lasts at least its gesture
  play_gesture(g);
  step_end_ms = now + (g_ms + 150 > st.ms ? g_ms + 150 : st.ms);
}
void play_seq(const Step* steps, int len, const char* name, unsigned long now) {
  seq = steps; seq_len = len; seq_i = 0; seq_name = name;
  start_step(now);
}
void service_seq(unsigned long now) {
  if (seq == nullptr || (long)(now - step_end_ms) < 0) return;
  if (++seq_i >= seq_len) { seq = nullptr; seq_name = ""; return; }
  start_step(now);
}

// --- Conditions: what the kids are doing ----------------------------------------
// Each condition has hysteresis (separate enter/exit tests) plus a short dwell, so a
// wobbly reading can't flicker the face. Higher enum value = higher priority.
enum Cond { K_NONE, K_DRY, K_WET, K_COLD, K_BREATH, K_HAND_NEAR, K_HAND_CLOSE, K_LOVE,
            K_BRIGHT, K_DARK, K_POKED, K_COUNT };
const char* kCondName[] = {"none","dry","wet","cold","breath","hand_near","hand_close","love",
                           "bright","dark","poked"};
struct Reaction { int8_t face; int8_t gest; int8_t card; };
const Reaction kReact[K_COUNT] = {
  {F_IDLE, G_NONE, -1},                       // none
  {F_THIRSTY, G_SLUMP, C_SOIL},               // dry: probe out in the air
  {F_TOO_WET, G_NUDGE, C_SOIL},               // wet: probe in wet soil
  {F_FREEZING, G_SHIVER, C_TEMP},             // cold
  {F_SWEATING, G_FAN, C_HUM},                 // breath on the thermo sensor
  {F_HAPPY, G_WAVE, C_DIST},                  // hand in front
  {F_SURPRISED, G_NUDGE_SOFT, C_DIST},        // hand very close: slow, gentle
  {F_LOVE, G_SWAY, -1},                       // hand stays
  {F_TOO_BRIGHT, G_SHADE, C_LIGHT},           // torch
  {F_SLEEPY, G_SLUMP, C_LIGHT},               // light sensor covered
  {F_ANGRY, G_NUDGE_SOFT, -1},                // poked 3x
};
// Rebase-able (baseline) conditions clear by accepting the new normal; the rest nag.
bool cond_rebases(int k) { return k == K_COLD || k == K_BREATH || k == K_BRIGHT || k == K_DARK; }

bool on[K_COUNT];
unsigned long on_since[K_COUNT], pend_since[K_COUNT], last_react[K_COUNT], last_nag[K_COUNT];
int top = K_NONE;
unsigned long hand_since = 0, poked_until = 0, pokes[3] = {0, 0, 0};
bool hand_was = false, jab = false;

bool cond_want(int k) {
  const bool o = on[k];
  const float d = s.dist_mm;
  const bool dv = !isnan(d);
  switch (k) {
    case K_DRY:   return o ? s.soil_pct < 8 : s.soil_pct < 3;
    case K_WET:   return o ? s.soil_pct > 60 : s.soil_pct > 70;
    case K_COLD:  return !isnan(base_temp) && !isnan(s.temp_c) &&
                         (o ? s.temp_c < base_temp - 1.5f : s.temp_c < base_temp - 2.5f);
    case K_BREATH:return !isnan(base_hum) && !isnan(s.hum_pct) &&
                         (o ? s.hum_pct > base_hum + 4 : s.hum_pct > base_hum + 8);
    case K_HAND_NEAR:  return dv && (o ? d < 480 : d < 400);
    case K_HAND_CLOSE: return dv && (o ? d < 150 : d < 120);
    case K_BRIGHT: if (isnan(base_lux) || s.lux < 0) return false;
                   return o ? s.lux > fmaxf(base_lux * 2.0f, base_lux + 250)
                            : s.lux > fmaxf(base_lux * 3.0f, base_lux + 400);
    case K_DARK:   if (isnan(base_lux) || base_lux < 15 || s.lux < 0) return false;
                   return o ? s.lux < base_lux * 0.45f : s.lux < base_lux * 0.25f;
    default: return false;
  }
}

void update_conditions(unsigned long now) {
  if (now < kWarmupMs) {                 // first readings can be junk: track them fast, react later
    ema(base_lux, s.lux < 0 ? NAN : s.lux, 0.3f); ema(base_hum, s.hum_pct, 0.3f); ema(base_temp, s.temp_c, 0.3f);
    return;
  }
  for (int k = K_DRY; k < K_COUNT; k++) {
    if (k == K_LOVE || k == K_POKED) continue;              // derived below
    bool want = cond_want(k);
    if (want == on[k]) { pend_since[k] = 0; continue; }
    if (pend_since[k] == 0) pend_since[k] = now;
    if (now - pend_since[k] >= (want ? 60UL : 600UL)) {      // dwell: snap in, slower out
      on[k] = want; on_since[k] = now; pend_since[k] = 0;
    }
  }
  // Pokes: quick jabs toward the distance sensor, read raw (no dwell) so a fast
  // in-and-out counts, and a kid standing within hand range doesn't hide it.
  const bool dv = !isnan(s.dist_mm);
  if (!jab && dv && s.dist_mm < 100) {
    jab = true;
    pokes[0] = pokes[1]; pokes[1] = pokes[2]; pokes[2] = now;
    if (pokes[0] && now - pokes[0] < 10000) { poked_until = now + 4000; pokes[0] = pokes[1] = pokes[2] = 0; }
  } else if (jab && (!dv || s.dist_mm > 160)) {
    jab = false;
  }
  // Love: a hand held in front.
  bool hand = on[K_HAND_NEAR] || on[K_HAND_CLOSE];
  if (hand && !hand_was) hand_since = now;
  hand_was = hand;
  bool love = hand && now - hand_since >= kLoveMs;
  if (love != on[K_LOVE]) { on[K_LOVE] = love; on_since[K_LOVE] = now; }
  bool poked = (long)(poked_until - now) > 0;
  if (poked != on[K_POKED]) { on[K_POKED] = poked; on_since[K_POKED] = now; }

  // Baselines adapt only while nothing is going on.
  bool calm = true;
  for (int k = K_DRY; k < K_COUNT; k++) if (on[k] && cond_rebases(k)) calm = false;
  if (calm) { ema(base_lux, s.lux < 0 ? NAN : s.lux, 0.004f);   // ~10 s time constant at 25 Hz
              ema(base_hum, s.hum_pct, 0.004f); ema(base_temp, s.temp_c, 0.004f); }

  // Long-held conditions go stale: env ones become the new normal after kRebaseMs.
  for (int k = K_DRY; k < K_COUNT; k++) {
    if (!on[k] || !cond_rebases(k) || now - on_since[k] < kRebaseMs) continue;
    base_lux = s.lux < 0 ? base_lux : s.lux; base_hum = s.hum_pct; base_temp = s.temp_c;
    on[k] = false; on_since[k] = now;
  }
}

// Highest-priority condition that is on and not stale.
int pick_top(unsigned long now) {
  for (int k = K_COUNT - 1; k > K_NONE; k--)
    if (on[k] && now - on_since[k] < kStaleMs) return k;
  return K_NONE;
}

void react(int k, unsigned long now) {
  const Reaction& r = kReact[k];
  react_buf[0] = {r.face, r.gest, -1, (uint16_t)kReactMs};
  react_buf[1] = {r.face, G_NONE, r.card, (uint16_t)kCardMs};
  play_seq(react_buf, r.card >= 0 ? 2 : 1, kCondName[k], now);
  last_react[k] = now; last_nag[k] = now;
}

// --- Idle personality ----------------------------------------------------------
unsigned long next_skit_ms = 15000, next_blink_ms = 3000;
int last_skit = -1;

void blink() { draw_eyes(true); delay(90); draw_eyes(false); }

void idle_life(unsigned long now) {
  bool resting = seq == nullptr && top == K_NONE && card < 0;
  if (!resting) return;

  if ((long)(now - next_skit_ms) >= 0) {
    int i; do { i = random(kSkitCount); } while (i == last_skit && kSkitCount > 1);
    last_skit = i;
    play_seq(kSkits[i].steps, kSkits[i].len, kSkits[i].name, now);
    next_skit_ms = now + random(kSkitMinMs, kSkitMaxMs);
  }
}

// --- Bridge: telemetry only (read by the passive logger in python/main.py) ---------
String read_status() {
  JsonDocument doc;
  doc["temp_c"] = s.temp_c; doc["hum_pct"] = s.hum_pct; doc["lux"] = s.lux;
  doc["dist_mm"] = s.dist_mm; doc["soil_pct"] = s.soil_pct; doc["soil_raw"] = s.soil_raw;
  doc["base_lux"] = base_lux; doc["base_hum"] = base_hum; doc["base_temp"] = base_temp;
  doc["face"] = kFaceName[face]; doc["card"] = card >= 0 ? kCardLabel[card] : "";
  doc["cond"] = kCondName[top]; doc["seq"] = seq_name; doc["arm"] = arm_last;
  JsonArray a = doc["on"].to<JsonArray>();
  for (int k = K_DRY; k < K_COUNT; k++) if (on[k]) a.add(kCondName[k]);
  doc["next_skit_s"] = (long)(next_skit_ms - millis()) / 1000;
  String out; serializeJson(doc, out); return out;
}

// --- Main ----------------------------------------------------------------------
void setup() {
  Bridge.begin();
  Modulino.begin();
  thermo_ok = thermo.begin(); light_ok = light.begin(); distance_ok = distance.begin();
  pinMode(kSoilPin, INPUT);
  tft.begin(); tft.setRotation(0); tft.setTextWrap(false);
  randomSeed(analogRead(kSoilPin) ^ micros());
  Bridge.provide_safe("read_status", read_status);

  // Boot: say hi while the baselines settle (see kWarmupMs).
  unsigned long now = millis();
  play_seq(kSkitHello, 1, "hello", now);
  next_skit_ms = now + 15000;
}

void loop() {
  const unsigned long now = millis();

  static unsigned long last_poll_ms = 0;
  if (now - last_poll_ms >= 40) {
    last_poll_ms = now;
    poll_sensors();
    update_conditions(now);

    int t = pick_top(now);
    if (t != top) {
      top = t;
      if (t != K_NONE) {
        if (now - last_react[t] > kRepeatMs || last_react[t] == 0) react(t, now);
        else { seq = nullptr; show_face((Face)kReact[t].face); }
      }
      next_skit_ms = now + random(kSkitMinMs, kSkitMaxMs);   // skits wait for quiet
    }
    // Stale soil/hand conditions (resting face back to idle) nag now and then.
    for (int k = K_DRY; k < K_COUNT; k++)
      if (on[k] && !cond_rebases(k) && now - on_since[k] >= kStaleMs &&
          now - last_nag[k] >= kNagMs && seq == nullptr && top == K_NONE) { react(k, now); break; }
  }

  service_seq(now);
  if (seq == nullptr) {
    show_face((Face)kReact[top].face);                   // resting face for the moment
    if (top == K_NONE && arm_g == nullptr && fabsf(arm_pos - kArmRest) > 1) play_gesture(G_HOME);
  }
  idle_life(now);

  if (card >= 0) {
    static unsigned long last_card_ms = 0;
    if (now - last_card_ms >= 250) { last_card_ms = now; card_refresh(false); }
  } else {
    if (face_dirty) { enter_face(); face_dirty = false; }
    animate_face(now);
    if ((long)(now - next_blink_ms) >= 0) {
      if (arm_g == nullptr && blink_ok(face)) blink();
      next_blink_ms = now + random(3500, 7000);
    }
  }

  service_arm(now);
}

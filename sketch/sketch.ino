// SPDX-License-Identifier: MIT
//
// Clawrophyll -- MCU firmware. Owns all real-time and safety-critical hardware,
// exposed to the Linux side as three Bridge RPC handlers:
//
// - read_status:    polls the Modulino sensors + the capacitive soil probe (A0)
//                   and returns everything as JSON.
// - set_expression: Phil's face -- GC9A01 round display via Adafruit_GC9A01A over
//                   HARDWARE SPI (SPI2 on D13=SCK / D11=MOSI). Two expressive eyes
//                   (color + shape per expression) + per-expression indicators +
//                   auto-blink. Ten states, validated here.
// - wave_arm:       SG90 servo arm on D3 -- a non-blocking, eased gesture engine.
//                   arm_angle() clamps every commanded angle to [kArmMin, kArmMax],
//                   so nothing upstream (agent, tool, bug) can move the arm past
//                   the safe range. The safety limit lives HERE, in firmware.
//
// Anti-clipping design: the eye "erase box" hugs the eyes tightly (y 60..160),
// and decorations live in the margins outside it. Text wrap is disabled so
// labels/ZZZ never wrap to the far side.
//
// kGalleryMode: dev aid -- auto-cycles every expression with its name labeled
// (used to tune the faces, and to film the gallery shots). Set false for normal
// (agent-driven) operation.

#include <Arduino_RouterBridge.h>
#include <Arduino_Modulino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Servo.h>

const int kSoilPin = A0;
const int kTftDC = 7, kTftCS = 10, kTftRST = 8;
Adafruit_GC9A01A tft(&SPI, kTftDC, kTftCS, kTftRST);

const uint16_t kBlack = 0x0000, kWhite = 0xFFFF, kCyan = 0x07FF, kGreen = 0x07E0;
const uint16_t kAmber = 0xFD20, kOrange = 0xFBE0, kIce = 0xAEFF, kDim = 0x03ED;
const uint16_t kDimBlu = 0x4A49, kYellow = 0xFFE0, kWater = 0x051F;
const uint16_t kRed = 0xF800, kSoil = 0xCB40;

const bool kGalleryMode = false;  // gallery tuning done; face is agent-driven via set_expression

const int kSoilDry = 944, kSoilWet = 594;
struct Sensors {
  float air_temp_c = NAN, humidity_pct = NAN, distance_mm = NAN, soil_pct = NAN;
  int light_lux = -1, light_raw = -1, soil_raw = -1;
};
Sensors sensors;
bool thermo_ok=false, light_ok=false, distance_ok=false, display_ok=false;
ModulinoThermo thermo; ModulinoLight light; ModulinoDistance distance;

String current_expression = "idle";
bool   face_dirty = true;

const int kEyeW = 68, kEyeH = 84, kEyeR = 26, kEyeCY = 108, kLeftCX = 76, kRightCX = 164;

struct EyeP { uint16_t color; float h; float w; int yoff; };
EyeP eye_params(const String& e) {
  if (e == "happy")      return {kGreen,  0.55f, 1.00f, 0};
  if (e == "sleepy")     return {kDimBlu, 0.20f, 1.00f, 8};
  if (e == "angry")      return {kRed,    0.70f, 1.00f, 4};
  if (e == "thirsty")    return {kAmber,  0.50f, 0.90f, 4};
  if (e == "sweating")   return {kOrange, 1.00f, 1.00f, 0};
  if (e == "freezing")   return {kIce,    0.60f, 0.72f, 0};
  if (e == "too_wet")    return {kCyan,   0.90f, 1.00f, 0};
  if (e == "too_dark")   return {kDim,    0.13f, 1.00f, 0};
  if (e == "too_bright") return {kOrange, 0.30f, 0.95f, 0};
  return {kCyan, 1.00f, 1.00f, 0};
}

bool blink_allowed() {
  String& e = current_expression;
  return !(e=="sleepy" || e=="freezing" || e=="too_wet" || e=="too_dark" || e=="too_bright");
}

// --- Servo arm (D3) ----------------------------------------------------------
// Non-blocking gesture engine, mirroring the face animate() state machine:
// wave_arm() selects a gesture; service_arm() in loop() eases the servo through
// its waypoints, so the face keeps animating and the sensors keep polling while
// the arm moves. The servo is attached only while a gesture plays, then detached
// -- limp, silent, no idle current. (A 100uF cap at the servo's 5V connector
// absorbs the SG90 inrush, so it shares the board 5V rail.)
//
// Motion is EASED: instead of commanding the servo straight to a waypoint (which
// slews at the SG90's full speed and looks snappy), we move it step_deg degrees
// per 20 ms servo frame toward the target. Higher step_deg = faster (the SG90's
// per-frame max is ~12 deg); smaller values (slump) read as slower / wearier.
// Each gesture sets its own. Blinks are suppressed while a gesture plays so the
// eye-redraw's blocking delay can't hitch the motion.
//
// kArmMirror flips the swing direction (CW <-> CCW) about rest, so the same
// gestures work whether the arm is mounted as Phil's left or right. The safe
// clamp is symmetric about rest so a flipped gesture reaches the same extent.
const bool kArmMirror = false;           // true = mirror every angle about kArmRest
const int kServoPin = 3;
Servo arm;
const int kArmRest = 90;                 // neutral / home angle (tunable)
const int kArmMin = 20, kArmMax = 160;   // safe clamp, symmetric about rest (90 +/- 70)
const int kArmStepMs = 20;               // ms per micro-step = the servo's 50 Hz frame

struct Waypoint { int angle; int hold_ms; };  // ease to angle, then hold before next
const Waypoint kWave[]  = {{150,60},{110,0},{150,50},{110,0},{150,50},{kArmRest,120}};
const Waypoint kNudge[] = {{55,50},{108,40},{55,50},{108,40},{kArmRest,110}};
const Waypoint kSlump[] = {{105,100},{125,130},{138,160},{142,280}};  // droops away from rest (mirrored from the old 75/55/42/38, which slumped the wrong way on the mounted arm)
const Waypoint kIdle[]  = {{kArmRest,120}};

// A gesture = its waypoints + a slew speed (degrees per 20 ms servo frame).
struct Gesture { const Waypoint* wp; int len; int step_deg; };
const Gesture kGWave  = {kWave,  (int)(sizeof(kWave)/sizeof(kWave[0])),   9}; // lively
const Gesture kGNudge = {kNudge, (int)(sizeof(kNudge)/sizeof(kNudge[0])), 10}; // sharp, quick
const Gesture kGSlump = {kSlump, (int)(sizeof(kSlump)/sizeof(kSlump[0])),  6}; // weary, slower
const Gesture kGIdle  = {kIdle,  (int)(sizeof(kIdle)/sizeof(kIdle[0])),    8}; // brisk

const Gesture* arm_g = nullptr;          // active gesture, or null when at rest
int  arm_step = 0;
int  arm_pos = kArmRest;                  // last commanded (output) angle, eased toward target
bool arm_attached = false;
unsigned long arm_next_ms = 0;
String arm_gesture = "idle";             // last gesture selected (reported in status)

// Map a gesture's logical angle to a servo command: apply the direction flip,
// then clamp to the safe range.
int arm_angle(int a) {
  if (kArmMirror) a = 2 * kArmRest - a;
  return a < kArmMin ? kArmMin : (a > kArmMax ? kArmMax : a);
}

// Ease toward the current waypoint step_deg degrees per frame; hold once reached,
// then advance. Detach (relax) when the sequence ends.
void service_arm(unsigned long now) {
  if (arm_g == nullptr) return;
  if (now < arm_next_ms) return;
  if (!arm_attached) {                            // attach + establish known position
    arm.attach(kServoPin); arm_attached = true;
    arm.write(arm_pos);
    arm_next_ms = now + kArmStepMs;
    return;
  }
  if (arm_step >= arm_g->len) {                   // finished -> relax
    arm.detach(); arm_attached = false;
    arm_g = nullptr;
    return;
  }
  int target = arm_angle(arm_g->wp[arm_step].angle);
  if (arm_pos != target) {                        // ease toward the waypoint
    int d = target - arm_pos, s = arm_g->step_deg;
    if (d >  s) d =  s;
    if (d < -s) d = -s;
    arm_pos += d;
    arm.write(arm_pos);
    arm_next_ms = now + kArmStepMs;
  } else {                                        // reached it -> hold, then next
    arm_next_ms = now + arm_g->wp[arm_step].hold_ms;
    arm_step++;
  }
}

// Select a gesture. Latest wins: replaces any in-progress gesture immediately.
bool wave_arm(String gesture) {
  if      (gesture == "wave")  arm_g = &kGWave;
  else if (gesture == "nudge") arm_g = &kGNudge;
  else if (gesture == "slump") arm_g = &kGSlump;
  else if (gesture == "idle")  arm_g = &kGIdle;
  else return false;
  arm_gesture = gesture;
  arm_step = 0; arm_next_ms = 0;                  // start easing on the next loop tick
  return true;
}

// thickened line: offset copies in x and y
void thick_line(int x0,int y0,int x1,int y1,int t,uint16_t c){
  for (int o = -t/2; o <= t/2; o++) {
    tft.drawLine(x0, y0+o, x1, y1+o, c);
    tft.drawLine(x0+o, y0, x1+o, y1, c);
  }
}

// filled 5-point star: central pentagon (fan over inner pts) + 5 arm triangles
void fill_star(int cx, int cy, int R, uint16_t col) {
  int ox[5], oy[5], ix[5], iy[5];
  for (int k = 0; k < 5; k++) {
    float ao = -PI/2 + k * (2*PI/5);
    float ai = ao + PI/5;
    ox[k] = cx + (int)(R * cos(ao));        oy[k] = cy + (int)(R * sin(ao));
    ix[k] = cx + (int)(R * 0.40f * cos(ai)); iy[k] = cy + (int)(R * 0.40f * sin(ai));
  }
  for (int k = 1; k < 4; k++)
    tft.fillTriangle(ix[0],iy[0], ix[k],iy[k], ix[k+1],iy[k+1], col);   // pentagon
  for (int k = 0; k < 5; k++)
    tft.fillTriangle(ox[k],oy[k], ix[k],iy[k], ix[(k+4)%5],iy[(k+4)%5], col); // arms
}

void draw_flake(int x, int y, uint16_t c) {
  tft.fillCircle(x, y, 3, c);
  tft.drawLine(x-6, y, x+6, y, c); tft.drawLine(x, y-6, x, y+6, c);
  tft.drawLine(x-4, y-4, x+4, y+4, c); tft.drawLine(x-4, y+4, x+4, y-4, c);
}

// Angry overlay: slant the eye tops (black wedges) + thick red brows merging in.
void overlay_angry() {
  const int yt = 83;
  tft.fillTriangle(110, yt, 110, yt+30, 56, yt, kBlack);   // left inner-top
  tft.fillTriangle(130, yt, 130, yt+30, 184, yt, kBlack);  // right inner-top
  for (int k = 0; k < 8; k++) {                            // thick brows
    tft.drawLine(38, 62+k, 112, 86+k, kRed);
    tft.drawLine(202, 62+k, 128, 86+k, kRed);
  }
}

void draw_eyes(bool blinking) {
  EyeP p = eye_params(current_expression);
  float hf = blinking ? 0.05f : p.h;
  int w = (int)(kEyeW * p.w);
  int h = (int)(kEyeH * hf); if (h < 5) h = 5;
  int r = kEyeR; if (r > w/2) r = w/2; if (r > h/2) r = h/2;
  const int cxs[2] = {kLeftCX, kRightCX};
  for (int i = 0; i < 2; i++) {
    int cx = cxs[i];
    tft.fillRect(cx - 40, kEyeCY - 48, 80, 100, kBlack);
    tft.fillRoundRect(cx - w/2, kEyeCY - h/2 + p.yoff, w, h, r, p.color);
  }
  if (current_expression == "angry") overlay_angry();
}

void deco_happy() { fill_star(52,40,18,kYellow); fill_star(120,28,15,kWhite);
                    fill_star(188,40,18,kYellow); }
void deco_thirsty(){ tft.fillRect(0,172,240,68,kSoil);
                     thick_line(30,172,70,230,5,kBlack);  thick_line(70,230,120,188,5,kBlack);
                     thick_line(120,188,175,232,5,kBlack);thick_line(120,188,205,180,5,kBlack);
                     thick_line(95,202,140,216,5,kBlack); }
void deco_sun() {    tft.fillCircle(240,-15,100,kYellow);           // bigger top-right corner sun
                     int ang[4] = {95,120,145,170};
                     for (int n=0;n<4;n++){ float r=ang[n]*PI/180.0f;
                       thick_line(240+(int)(100*cos(r)), -15+(int)(100*sin(r)),
                                  240+(int)(124*cos(r)), -15+(int)(124*sin(r)), 3, kYellow); } }
void deco_text(const char* s,int x,int y,int sz,uint16_t c){
  tft.setTextColor(c); tft.setTextSize(sz); tft.setCursor(x,y); tft.print(s); }

void draw_static_decor(const String& e) {
  if (e == "happy")        deco_happy();
  else if (e == "thirsty") deco_thirsty();
  else if (e == "too_bright") deco_sun();
  else if (e == "freezing") deco_text("Brrr", 92, 198, 2, kIce);
}

void draw_label(const String& name) {
  tft.fillRect(0,192,240,28,kBlack);
  tft.setTextColor(kWhite); tft.setTextSize(2);
  int x = 120 - (int)(name.length()*6); if (x < 4) x = 4;
  tft.setCursor(x,196); tft.print(name);
}

unsigned long expr_start_ms = 0, last_anim_ms = 0;
int sweat_y = 0, water_level = 0, zzz_n = 0;
int flake_x[3] = {26,120,214}, flake_y[3] = {-10,-50,-90};

void enter_expression() {
  tft.fillScreen(kBlack);
  draw_eyes(false);
  draw_static_decor(current_expression);
  if (kGalleryMode && current_expression != "freezing") draw_label(current_expression);
  expr_start_ms = millis();
  sweat_y = 66; water_level = 0; zzz_n = 0;
  flake_y[0] = -10; flake_y[1] = -50; flake_y[2] = -90;
}

void animate(unsigned long now) {
  if (now - last_anim_ms < 90) return;
  last_anim_ms = now;
  const String& e = current_expression;

  if (e == "sweating") {
    tft.fillRect(206, sweat_y-26, 34, 38, kBlack);
    sweat_y += 12; if (sweat_y > 156) sweat_y = 66;
    int dx = 218, dy = sweat_y;
    tft.fillCircle(dx, dy, 10, kWater);
    tft.fillTriangle(dx-10, dy-4, dx+10, dy-4, dx, dy-24, kWater);
  } else if (e == "freezing") {
    for (int i = 0; i < 3; i++) {
      tft.fillRect(flake_x[i]-8, flake_y[i]-8, 17, 17, kBlack);
      flake_y[i] += 5 + i; if (flake_y[i] > 232) flake_y[i] = -6;
      draw_flake(flake_x[i], flake_y[i], kWhite);
    }
  } else if (e == "too_wet") {
    if (water_level < 150) { water_level += 5; tft.fillRect(0, 240-water_level, 240, 6, kWater); }
    else if (now - expr_start_ms > 5000) enter_expression();
  } else if (e == "sleepy") {
    if (now - expr_start_ms > (unsigned long)(700 + zzz_n*600)) {
      zzz_n = (zzz_n % 3) + 1;
      tft.fillRect(144, 22, 86, 74, kBlack);
      if (zzz_n >= 1) deco_text("z", 150, 78, 2, kIce);
      if (zzz_n >= 2) deco_text("z", 168, 54, 3, kIce);
      if (zzz_n >= 3) deco_text("Z", 182, 30, 4, kIce);
      if (zzz_n == 3) expr_start_ms = now;
    }
  }
}

String read_status() {
  JsonDocument doc;
  doc["air_temp_c"]=sensors.air_temp_c; doc["humidity_pct"]=sensors.humidity_pct;
  doc["light_lux"]=sensors.light_lux;   doc["light_raw"]=sensors.light_raw;
  doc["distance_mm"]=sensors.distance_mm;
  doc["soil_moisture_pct"]=sensors.soil_pct; doc["soil_moisture_raw"]=sensors.soil_raw;
  doc["expression"]=current_expression; doc["arm_gesture"]=arm_gesture;
  doc["thermo_ok"]=thermo_ok; doc["light_ok"]=light_ok;
  doc["distance_ok"]=distance_ok; doc["display_ok"]=display_ok;
  String out; serializeJson(doc, out); return out;
}

bool set_expression(String name) {
  static const char* kValid[] = {"thirsty","sweating","freezing","too_wet","too_dark",
                                 "too_bright","happy","sleepy","angry","idle"};
  for (auto v : kValid) if (name == v) {
    if (name != current_expression) { current_expression = name; face_dirty = true; }
    return true;
  }
  return false;
}

void setup() {
  Bridge.begin();
  Modulino.begin();
  thermo_ok = thermo.begin(); light_ok = light.begin(); distance_ok = distance.begin();
  pinMode(kSoilPin, INPUT);
  tft.begin(); display_ok = true; tft.setRotation(0); tft.setTextWrap(false);
  tft.fillScreen(kBlack);
  Bridge.provide_safe("read_status", read_status);
  Bridge.provide_safe("set_expression", set_expression);
  Bridge.provide_safe("wave_arm", wave_arm);
  wave_arm("idle");  // home the arm to rest on boot, then it relaxes (detaches)
}

void loop() {
  const unsigned long now = millis();

  if (kGalleryMode) {
    static const char* kTour[] = {"idle","happy","sleepy","angry","thirsty",
                                  "sweating","freezing","too_wet","too_dark","too_bright"};
    static unsigned long next_tour_ms = 1500;
    static int ti = 0;
    if (now >= next_tour_ms) { ti = (ti+1)%10; current_expression = kTour[ti];
                               face_dirty = true; next_tour_ms = now + 7000; }
  }

  if (face_dirty) { enter_expression(); face_dirty = false; }
  animate(now);
  service_arm(now);

  static unsigned long next_blink_ms = 3000;
  if (now >= next_blink_ms) {
    // Skip blinks while a gesture plays -- the blink's blocking redraw would
    // freeze the arm mid-motion. Blinks resume once the arm is idle.
    if (arm_g == nullptr && blink_allowed()) { draw_eyes(true); delay(100); draw_eyes(false); }
    next_blink_ms = now + random(2600, 5200);
  }

  if (distance_ok && distance.available()) sensors.distance_mm = distance.get();

  static unsigned long last_poll_ms = 0;
  if (now - last_poll_ms >= 200) {
    last_poll_ms = now;
    if (thermo_ok) { sensors.air_temp_c=thermo.getTemperature(); sensors.humidity_pct=thermo.getHumidity(); }
    if (light_ok)  { light.update(); sensors.light_lux=light.getLux(); sensors.light_raw=light.getAL(); }
    int raw = analogRead(kSoilPin); sensors.soil_raw = raw;
    float pct = 100.0f*(float)(kSoilDry-raw)/(float)(kSoilDry-kSoilWet);
    if (pct<0) pct=0; if (pct>100) pct=100; sensors.soil_pct = pct;
  }
}

/*
 * DIMO — Desktop Robot Companion
 * Waveshare ESP32-C6 Touch AMOLED 1.8" (368x448)
 *
 * Phase 1 — ONE face (Cute), smooth animations, AMOLED colors.
 * Pure black background = OLED pixels OFF = perfect black.
 * Partial redraws only — zero flicker.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEHIDDevice.h>
#include <Adafruit_XCA9554.h>
#include "Arduino_GFX_Library.h"
#include "HWCDC.h"

HWCDC USBSerial;

// ── Hardware ──────────────────────────────────────────────────────────────────
#define SDA_PIN  8
#define SCL_PIN  7
#define SCR_W    368
#define SCR_H    448

Adafruit_XCA9554 expander;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(5,0,1,2,3,4);
Arduino_SH8601  *gfx = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, SCR_W, SCR_H);

// ── Credentials ───────────────────────────────────────────────────────────────
const char *WIFI_SSID = "Bassily - IoT";
const char *WIFI_PASS = "@Bassily199711412";
const char *OWM_URL   = "http://api.openweathermap.org/data/2.5/weather"
                        "?q=Cumming,GA,US&units=imperial"
                        "&appid=898dec73df9c4e262a862baa0c11028f";
const char *TZ_INFO   = "EST5EDT,M3.2.0/2,M11.1.0/2";

// ── AMOLED color palette ───────────────────────────────────────────────────────
// Pure black = pixels OFF on AMOLED (perfect contrast)
#define BLACK       0x0000
#define WHITE       0xFFFF
#define OFF_WHITE   0xEF7B   // slightly warm white
#define IRIS        0x10A3   // dark blue-gray iris
#define PUPIL       0x0000   // pure black pupil
#define SHINE1      0xFFFF   // primary shine
#define SHINE2      0xD6FB   // secondary shine (cool white)
#define LID_GRAY    0x4228   // eyelid/lash color
#define CHEEK       0xF8B4   // soft pink cheek
#define CHEEK2      0xFBD6   // inner cheek (lighter)
#define SMILE_COL   0x8C51   // warm dark for mouth
#define CYAN_GLOW   0x07FF
#define SPARK_COL   0xD6FB   // sparkle dots above eyes
#define DIM         0x2965
#define GRAY        0x7BEF
#define GREEN       0x07E0
#define RED_COL     0xF800
#define YELLOW      0xFFE0
#define CYAN        0x07FF
#define PURPLE      0xC81F
#define ORANGE      0xFD20

// ── Face layout ───────────────────────────────────────────────────────────────
#define EL_CX    122    // left eye centre X
#define ER_CX    246    // right eye centre X
#define E_CY     196    // eye centre Y
#define EYE_R    44     // eye outer radius
#define CK_LX    76     // left cheek X
#define CK_RX    292    // right cheek X
#define CK_Y     256    // cheek Y
#define MOUTH_X  184    // mouth centre X
#define MOUTH_Y  318    // mouth centre Y
#define SBAR_H   30     // status bar height

// ── Pages ─────────────────────────────────────────────────────────────────────
enum Page { PAGE_FACE, PAGE_CLOCK, PAGE_WEATHER, PAGE_MUSIC };
Page page = PAGE_FACE;

// ── Animation state ───────────────────────────────────────────────────────────
float    blinkPhase   = 0.0f;   // 0=open, 1=closed
bool     isBlinking   = false;
uint32_t blinkTimer   = 0;
uint32_t nextBlink    = 4000;

float    shimmerAngle = 0.0f;   // rotating shine highlight
uint32_t lastShimmer  = 0;

int8_t   eyeOX=0, eyeOY=0;
uint32_t nextEyeMove  = 3000;
uint32_t eyeMoveTimer = 0;

uint32_t sparkTimer   = 0;
uint8_t  sparkFrame   = 0;

// ── Dirty flags ───────────────────────────────────────────────────────────────
bool dirtyEyes    = true;
bool dirtyMouth   = true;
bool dirtyCheeks  = true;
bool dirtyStatus  = true;
bool dirtyFull    = true;

// ── WiFi / weather ────────────────────────────────────────────────────────────
bool     wifiOk   = false;
String   wxTemp   = "--";
String   wxDesc   = "";
bool     wxOk     = false;
bool     wxFetched= false;
uint32_t lastWx   = 0;

// ── Touch ─────────────────────────────────────────────────────────────────────
bool     tdDown   = false;
int16_t  tdSX=0, tdSY=0, tdLX=0, tdLY=0;
uint32_t tdMs     = 0;
bool     tdSwiped = false;

// ── BLE ───────────────────────────────────────────────────────────────────────
bool              bleConn   = false;
BLEHIDDevice     *bleHID    = nullptr;
BLECharacteristic*bleIn     = nullptr;
bool              blePlaying = false;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
void centered(const char *s, int16_t y, uint16_t col, uint8_t sz=1) {
  gfx->setTextSize(sz);
  gfx->setTextColor(col);
  gfx->setCursor((SCR_W - (int16_t)(strlen(s)*6*sz)) / 2, y);
  gfx->print(s);
}
float jsonF(const String &b, const char *k) {
  String key = String("\"")+k+"\":";
  int i=b.indexOf(key); if(i<0) return NAN;
  return b.substring(i+key.length(), i+key.length()+10).toFloat();
}
String jsonS(const String &b, const char *k) {
  String key = String("\"")+k+"\":\"";
  int i=b.indexOf(key); if(i<0) return "";
  int s=i+key.length(), e=b.indexOf('"',s);
  return b.substring(s,e);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch (FT3168 I2C 0x38)
// ─────────────────────────────────────────────────────────────────────────────
bool touchRead(int16_t &x, int16_t &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(0x38, 6);
  if (Wire.available() < 6) return false;
  uint8_t td=Wire.read(), xh=Wire.read(), xl=Wire.read(),
              yh=Wire.read(), yl=Wire.read();
  Wire.read();
  if ((td & 0x0F) == 0) return false;
  if ((xh & 0xC0) == 0x80) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  EYE DRAWING  — the heart of DIMO
//  openRatio: 1.0 = fully open, 0.0 = fully closed
//  Shine rotates with shimmerAngle for live sparkle effect
// ─────────────────────────────────────────────────────────────────────────────
void drawEye(int16_t cx, int16_t cy, float openRatio, int8_t ox, int8_t oy) {
  int16_t R  = EYE_R;
  int16_t px = cx + ox;
  int16_t py = cy + oy;

  // Clear eye area (pure black = AMOLED pixels OFF)
  gfx->fillRect(cx - R - 8, cy - R - 10, (R+8)*2, (R+10)*2 + 12, BLACK);

  if (openRatio <= 0.05f) {
    // Fully closed — thin rounded line
    gfx->fillRoundRect(px - R + 4, py - 3, (R-4)*2, 7, 3, LID_GRAY);
    return;
  }

  // ── Layers from outside in (AMOLED: vivid on pure black) ──────────────────

  // 1. Subtle outer glow (very faint blue — depth against black)
  gfx->fillCircle(px, py, R + 4, 0x0208);

  // 2. White sclera
  gfx->fillCircle(px, py, R, WHITE);

  // 3. Off-white inner sclera (depth)
  gfx->fillCircle(px, py, R - 4, OFF_WHITE);

  // 4. Dark iris
  int16_t irisR = 27;
  gfx->fillCircle(px, py, irisR, IRIS);

  // 5. Deeper inner iris ring
  gfx->fillCircle(px, py, irisR - 6, 0x0861);

  // 6. Pupil
  gfx->fillCircle(px, py, 14, PUPIL);

  // 7. Rotating primary shine
  float sa = shimmerAngle;
  int16_t sx1 = px + (int16_t)(12 * cos(sa));
  int16_t sy1 = py - (int16_t)(12 * sin(sa)) - 6;
  gfx->fillCircle(sx1, sy1, 7, SHINE1);

  // 8. Secondary smaller shine (opposite angle)
  int16_t sx2 = px - (int16_t)(7  * cos(sa));
  int16_t sy2 = py + (int16_t)(7  * sin(sa)) + 4;
  gfx->fillCircle(sx2, sy2, 4, SHINE2);

  // 9. Eyelid close animation (top-down black overlay)
  if (openRatio < 1.0f) {
    int16_t coverH = (int16_t)((1.0f - openRatio) * (R * 2 + 6));
    int16_t topY   = py - R - 3;
    gfx->fillRect(px - R - 2, topY, (R+2)*2, coverH, BLACK);
    // Eyelid edge — soft arc line
    int16_t lidY = topY + coverH;
    for (int xi = -(R-2); xi <= (R-2); xi++) {
      float curve = (float)(xi * xi) * 3.0f / ((R-2)*(R-2));
      gfx->drawPixel(px + xi, lidY + (int16_t)curve, LID_GRAY);
      gfx->drawPixel(px + xi, lidY + (int16_t)curve + 1, LID_GRAY);
    }
  }

  // 10. Top lash line (always visible)
  if (openRatio > 0.3f) {
    for (int xi = -(R-2); xi <= (R-2); xi++) {
      float curve = (float)(xi * xi) * 3.0f / ((R-2)*(R-2));
      gfx->drawPixel(px + xi, py - R - 2 + (int16_t)curve, LID_GRAY);
      gfx->drawPixel(px + xi, py - R - 1 + (int16_t)curve, LID_GRAY);
    }
  }
}

// Sparkle marks above eyes (like reference cute face)
void drawSparkles(uint8_t frame) {
  // Clear sparkle zone
  gfx->fillRect(EL_CX-60, E_CY-80, 50, 30, BLACK);
  gfx->fillRect(ER_CX+10, E_CY-80, 50, 30, BLACK);

  // Pulse in/out with frame
  uint16_t col = (frame % 2 == 0) ? SPARK_COL : 0x8C6B;
  int16_t  sz  = (frame % 4 < 2) ? 3 : 4;

  auto star = [&](int16_t sx, int16_t sy) {
    gfx->fillCircle(sx, sy, sz, col);
    gfx->drawFastHLine(sx - sz*2, sy, sz*4+1, col);
    gfx->drawFastVLine(sx, sy - sz*2, sz*4+1, col);
  };

  star(EL_CX - 36, E_CY - 64);
  star(ER_CX + 36, E_CY - 64);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CHEEKS — soft pink gradient circles
// ─────────────────────────────────────────────────────────────────────────────
void drawCheeks() {
  gfx->fillRect(CK_LX-30, CK_Y-30, 60, 60, BLACK);
  gfx->fillRect(CK_RX-30, CK_Y-30, 60, 60, BLACK);

  // Outer soft pink
  gfx->fillCircle(CK_LX, CK_Y, 22, 0xF010);
  gfx->fillCircle(CK_RX, CK_Y, 22, 0xF010);
  // Mid
  gfx->fillCircle(CK_LX, CK_Y, 15, CHEEK);
  gfx->fillCircle(CK_RX, CK_Y, 15, CHEEK);
  // Inner bright
  gfx->fillCircle(CK_LX, CK_Y, 8,  CHEEK2);
  gfx->fillCircle(CK_RX, CK_Y, 8,  CHEEK2);
}

// ─────────────────────────────────────────────────────────────────────────────
//  MOUTH — small cute U smile
// ─────────────────────────────────────────────────────────────────────────────
void drawMouth() {
  // Clear mouth zone
  gfx->fillRect(MOUTH_X - 50, MOUTH_Y - 16, 100, 50, BLACK);

  // Small U-shaped smile — 3px thick arc
  int16_t r = 28, depth = 14;
  for (int x = -r; x <= r; x++) {
    int16_t y = (int16_t)((float)x*x * depth / (r*r));
    gfx->drawPixel(MOUTH_X+x, MOUTH_Y+y,   SMILE_COL);
    gfx->drawPixel(MOUTH_X+x, MOUTH_Y+y+1, SMILE_COL);
    gfx->drawPixel(MOUTH_X+x, MOUTH_Y+y+2, SMILE_COL);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  STATUS BAR — minimal, AMOLED-clean
// ─────────────────────────────────────────────────────────────────────────────
void drawStatusBar() {
  gfx->fillRect(0, 0, SCR_W, SBAR_H, BLACK);

  struct tm t;
  if (getLocalTime(&t, 30)) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(8, 11); gfx->print(buf);
  }

  // Tiny WiFi dot
  gfx->fillCircle(SCR_W/2, 15, 4, wifiOk ? GREEN : RED_COL);

  // Weather (tiny)
  if (wxOk) {
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    String s = wxTemp + "F";
    gfx->setCursor(SCR_W - s.length()*6 - 8, 11);
    gfx->print(s);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BOOT ANIMATION — 5 frames from reference
// ─────────────────────────────────────────────────────────────────────────────
void bootAnimation() {
  gfx->fillScreen(BLACK);
  delay(200);

  // Frame 1: Power On — tiny dots flicker in
  for (int r = 1; r <= 8; r += 2) {
    gfx->fillCircle(EL_CX, E_CY, r+2, 0x0208);
    gfx->fillCircle(EL_CX, E_CY, r,   0x0630);
    gfx->fillCircle(ER_CX, E_CY, r+2, 0x0208);
    gfx->fillCircle(ER_CX, E_CY, r,   0x0630);
    delay(60);
  }
  delay(300);

  // Frame 2: Eyes opening — grow from small to full
  for (int r = 6; r <= EYE_R; r += 4) {
    gfx->fillRect(EL_CX-EYE_R-10, E_CY-EYE_R-10, (EYE_R+10)*2, (EYE_R+10)*2, BLACK);
    gfx->fillRect(ER_CX-EYE_R-10, E_CY-EYE_R-10, (EYE_R+10)*2, (EYE_R+10)*2, BLACK);
    // Simple growing white circles
    gfx->fillCircle(EL_CX, E_CY, r+4, 0x0208);
    gfx->fillCircle(EL_CX, E_CY, r, WHITE);
    gfx->fillCircle(EL_CX, E_CY, r-6, IRIS);
    gfx->fillCircle(EL_CX, E_CY, max(0,r-14), BLACK);
    gfx->fillCircle(ER_CX, E_CY, r+4, 0x0208);
    gfx->fillCircle(ER_CX, E_CY, r, WHITE);
    gfx->fillCircle(ER_CX, E_CY, r-6, IRIS);
    gfx->fillCircle(ER_CX, E_CY, max(0,r-14), BLACK);
    delay(35);
  }
  delay(300);

  // Frame 3: Happy face fully rendered
  shimmerAngle = PI/4;
  drawEye(EL_CX, E_CY, 1.0f, 0, 0);
  drawEye(ER_CX, E_CY, 1.0f, 0, 0);
  drawMouth();
  drawCheeks();
  delay(500);

  // Frame 4: "DIMO" name
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE); gfx->setTextSize(5);
  int16_t tw = 4 * 6 * 5;
  gfx->setCursor((SCR_W - tw) / 2, SCR_H/2 - 40);
  gfx->print("DIMO");
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  centered("desktop friend", SCR_H/2 + 28, DIM, 1);
  delay(800);

  // Frame 5: Ready — full face + sparkles + cheek pop
  gfx->fillScreen(BLACK);
  shimmerAngle = PI/4;
  drawEye(EL_CX, E_CY, 1.0f, 0, 0);
  drawEye(ER_CX, E_CY, 1.0f, 0, 0);
  drawMouth();
  drawCheeks();
  drawSparkles(0);
  delay(600);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WIFI CONNECTING ANIMATION — 5 frames from reference
// ─────────────────────────────────────────────────────────────────────────────
void wifiConnectingAnimation() {
  auto wifiIcon = [](int16_t cx, int16_t cy, int bars, uint16_t col) {
    gfx->fillCircle(cx, cy, 5, col);
    if (bars >= 1) { gfx->drawCircle(cx,cy,16,col); gfx->drawCircle(cx,cy,17,col); }
    if (bars >= 2) { gfx->drawCircle(cx,cy,28,col); gfx->drawCircle(cx,cy,29,col); }
    if (bars >= 3) { gfx->drawCircle(cx,cy,40,col); gfx->drawCircle(cx,cy,41,col); }
    gfx->fillRect(cx-44,cy-44,88,44,BLACK); // mask upper half
  };

  auto lookLeft = [&]() {
    // Crescent eyes looking left
    gfx->fillRect(EL_CX-EYE_R-8, E_CY-EYE_R-8, (EYE_R+8)*2, (EYE_R+8)*2, BLACK);
    gfx->fillRect(ER_CX-EYE_R-8, E_CY-EYE_R-8, (EYE_R+8)*2, (EYE_R+8)*2, BLACK);
    gfx->fillCircle(EL_CX, E_CY, EYE_R, WHITE);
    gfx->fillCircle(EL_CX+16, E_CY, EYE_R-2, BLACK);
    gfx->fillCircle(ER_CX, E_CY, EYE_R, WHITE);
    gfx->fillCircle(ER_CX+16, E_CY, EYE_R-2, BLACK);
  };
  auto lookRight = [&]() {
    gfx->fillRect(EL_CX-EYE_R-8, E_CY-EYE_R-8, (EYE_R+8)*2, (EYE_R+8)*2, BLACK);
    gfx->fillRect(ER_CX-EYE_R-8, E_CY-EYE_R-8, (EYE_R+8)*2, (EYE_R+8)*2, BLACK);
    gfx->fillCircle(EL_CX, E_CY, EYE_R, WHITE);
    gfx->fillCircle(EL_CX-16, E_CY, EYE_R-2, BLACK);
    gfx->fillCircle(ER_CX, E_CY, EYE_R, WHITE);
    gfx->fillCircle(ER_CX-16, E_CY, EYE_R-2, BLACK);
  };

  // 1: Looking left
  gfx->fillScreen(BLACK);
  lookLeft();
  delay(450);

  // 2: Looking right + wifi bar 1
  gfx->fillScreen(BLACK);
  lookRight();
  wifiIcon(SCR_W/2, SCR_H-80, 1, DIM);
  delay(450);

  // 3: Centre + wifi bar 2
  gfx->fillScreen(BLACK);
  shimmerAngle = PI/4;
  drawEye(EL_CX, E_CY, 1.0f, 0, 0);
  drawEye(ER_CX, E_CY, 1.0f, 0, 0);
  wifiIcon(SCR_W/2, SCR_H-80, 2, CYAN);
  delay(450);

  // 4: Eyes up + wifi bar 3
  gfx->fillScreen(BLACK);
  drawEye(EL_CX, E_CY, 1.0f, 0, -10);
  drawEye(ER_CX, E_CY, 1.0f, 0, -10);
  wifiIcon(SCR_W/2, SCR_H-80, 3, CYAN);
  delay(400);

  // 5: Connected! — full cute face + green sparkle
  gfx->fillScreen(BLACK);
  drawEye(EL_CX, E_CY, 1.0f, 0, 0);
  drawEye(ER_CX, E_CY, 1.0f, 0, 0);
  drawMouth(); drawCheeks(); drawSparkles(0);
  gfx->setTextColor(GREEN); gfx->setTextSize(1);
  centered("Connected!", SCR_H-44, GREEN, 1);
  delay(700);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLOCK PAGE — reference style (time + date + sleeping DIMO mini)
// ─────────────────────────────────────────────────────────────────────────────
void drawClockPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  struct tm t;
  if (!getLocalTime(&t, 200)) {
    centered("Syncing...", SCR_H/2, DIM, 2);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(6, SCR_H-14); gfx->print("< face");
    gfx->setCursor(SCR_W-74, SCR_H-14); gfx->print("weather >");
    return;
  }

  // Big bold time
  char tBuf[8], dBuf[16];
  snprintf(tBuf, sizeof(tBuf), "%02d:%02d", t.tm_hour, t.tm_min);
  strftime(dBuf, sizeof(dBuf), "%a  %b %d", &t);

  gfx->setTextColor(WHITE); gfx->setTextSize(5);
  int16_t tw = strlen(tBuf) * 30;
  gfx->setCursor((SCR_W - tw) / 2, 148);
  gfx->print(tBuf);

  // Day + date
  gfx->setTextColor(CYAN); gfx->setTextSize(2);
  centered(dBuf, 228, CYAN, 2);

  // Seconds dot ring
  float ang = (t.tm_sec / 60.0f) * 2 * PI - PI/2;
  gfx->drawCircle(SCR_W/2, 338, 50, DIM);
  gfx->fillCircle(SCR_W/2 + (int)(50*cos(ang)),
                  338     + (int)(50*sin(ang)), 7, CYAN);

  // Small sleeping DIMO eyes above the time
  int16_t fy = 100;
  // tiny droopy eyes
  gfx->fillCircle(EL_CX, fy, 18, WHITE);
  gfx->fillRect(EL_CX-20, fy-20, 40, 20, BLACK);
  gfx->fillCircle(EL_CX, fy+4, 10, IRIS);
  gfx->fillCircle(ER_CX, fy, 18, WHITE);
  gfx->fillRect(ER_CX-20, fy-20, 40, 20, BLACK);
  gfx->fillCircle(ER_CX, fy+4, 10, IRIS);
  // flat mouth
  gfx->fillRoundRect(MOUTH_X-14, fy+26, 28, 5, 2, DIM);
  // ZZZ
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(ER_CX+22, fy-28); gfx->print("z");
  gfx->setTextSize(2);
  gfx->setCursor(ER_CX+34, fy-46); gfx->print("Z");
  gfx->setTextSize(3);
  gfx->setCursor(ER_CX+52, fy-72); gfx->print("Z");

  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6, SCR_H-14); gfx->print("< face");
  gfx->setCursor(SCR_W-74, SCR_H-14); gfx->print("weather >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  WEATHER PAGE — icon + temp + description
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  centered("Cumming, GA", 44, DIM, 1);

  if (!wifiOk) {
    // No wifi face
    gfx->fillCircle(EL_CX, E_CY-40, 22, WHITE);
    gfx->fillCircle(EL_CX, E_CY-40, 14, IRIS);
    gfx->fillCircle(ER_CX, E_CY-40, 22, WHITE);
    gfx->fillCircle(ER_CX, E_CY-40, 14, IRIS);
    // frown
    for(int x=-28;x<=28;x++) {
      int16_t y=-(int16_t)((float)x*x*12/(28*28))+16;
      gfx->drawPixel(MOUTH_X+x, E_CY-40+50+y, GRAY);
    }
    centered("No WiFi", SCR_H-120, RED_COL, 2);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(6,SCR_H-14); gfx->print("< clock");
    gfx->setCursor(SCR_W-52,SCR_H-14); gfx->print("face >");
    return;
  }
  if (!wxOk) {
    centered("Loading...", SCR_H/2, DIM, 2);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(6,SCR_H-14); gfx->print("< clock");
    gfx->setCursor(SCR_W-52,SCR_H-14); gfx->print("face >");
    return;
  }

  // Big temp
  String ts = wxTemp + " F";
  gfx->setTextColor(WHITE); gfx->setTextSize(5);
  gfx->setCursor((SCR_W - (int16_t)ts.length()*30) / 2, 72);
  gfx->print(ts);

  // Description
  String dc = wxDesc; if(dc.length()>0) dc[0]=toupper(dc[0]);
  centered(dc.c_str(), 148, CYAN, 2);

  // Large weather icon
  int16_t ix = SCR_W/2, iy = 270;
  if (wxDesc.indexOf("clear")>=0 || wxDesc.indexOf("sun")>=0) {
    gfx->fillCircle(ix, iy, 44, YELLOW);
    for (int a=0; a<360; a+=45) {
      float r=a*PI/180;
      gfx->drawLine(ix+(int)(55*cos(r)),iy+(int)(55*sin(r)),
                    ix+(int)(70*cos(r)),iy+(int)(70*sin(r)),YELLOW);
      gfx->drawLine(ix+(int)(56*cos(r)),iy+(int)(56*sin(r)),
                    ix+(int)(69*cos(r)),iy+(int)(69*sin(r)),YELLOW);
    }
  } else if (wxDesc.indexOf("rain")>=0 || wxDesc.indexOf("drizzle")>=0 ||
             wxDesc.indexOf("storm")>=0) {
    gfx->fillEllipse(ix, iy-12, 52, 32, GRAY);
    gfx->fillEllipse(ix-22, iy-26, 34, 22, GRAY);
    gfx->fillEllipse(ix+18, iy-28, 28, 20, GRAY);
    for (int d=-2; d<=2; d++) {
      gfx->fillRoundRect(ix+d*20-3, iy+26, 6, 24, 3, CYAN);
    }
  } else if (wxDesc.indexOf("snow")>=0) {
    gfx->fillEllipse(ix, iy-12, 52, 32, WHITE);
    gfx->fillEllipse(ix-22, iy-26, 34, 22, WHITE);
    for (int d=-2; d<=2; d++) {
      gfx->fillCircle(ix+d*24, iy+30, 6, WHITE);
    }
  } else {
    // Cloud
    gfx->fillEllipse(ix, iy, 52, 32, GRAY);
    gfx->fillEllipse(ix-22, iy-18, 34, 22, GRAY);
    gfx->fillEllipse(ix+18, iy-20, 28, 20, GRAY);
  }

  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6, SCR_H-14); gfx->print("< clock");
  gfx->setCursor(SCR_W-52, SCR_H-14); gfx->print("face >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  MUSIC PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawMusicPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  // Small neutral eyes
  int16_t fy = 130;
  gfx->fillCircle(EL_CX, fy, 24, WHITE);
  gfx->fillCircle(EL_CX, fy, 15, IRIS);
  gfx->fillCircle(EL_CX, fy, 7,  BLACK);
  gfx->fillCircle(EL_CX+6, fy-6, 5, WHITE);
  gfx->fillCircle(ER_CX, fy, 24, WHITE);
  gfx->fillCircle(ER_CX, fy, 15, IRIS);
  gfx->fillCircle(ER_CX, fy, 7,  BLACK);
  gfx->fillCircle(ER_CX+6, fy-6, 5, WHITE);
  // small smile
  for(int x=-20;x<=20;x++) {
    int16_t y=(int16_t)((float)x*x*8/(20*20));
    gfx->drawPixel(MOUTH_X+x, fy+36+y, SMILE_COL);
    gfx->drawPixel(MOUTH_X+x, fy+37+y, SMILE_COL);
  }

  if (bleConn) centered("Connected", 194, GREEN, 1);
  else         centered("Pair: DIMO Remote", 194, DIM, 1);

  // Audio bars
  int16_t by = 310;
  uint8_t h[] = {20,34,50,62,50,34,20};
  for (int i=0; i<7; i++) {
    uint16_t bc = (i==3) ? CYAN : (i==2||i==4) ? 0x03EF : DIM;
    gfx->fillRoundRect(SCR_W/2-54+i*16, by-h[i], 11, h[i], 3, bc);
  }

  // Controls
  int16_t cy2 = SCR_H-68;
  gfx->fillTriangle(58,cy2-20,58,cy2+20,36,cy2,WHITE);
  gfx->fillRect(34,cy2-20,7,40,WHITE);
  if (blePlaying) {
    gfx->fillRoundRect(SCR_W/2-19,cy2-22,14,44,2,GREEN);
    gfx->fillRoundRect(SCR_W/2+5, cy2-22,14,44,2,GREEN);
  } else {
    gfx->fillTriangle(SCR_W/2-14,cy2-22,SCR_W/2-14,cy2+22,SCR_W/2+18,cy2,GREEN);
  }
  gfx->fillTriangle(310,cy2-20,310,cy2+20,332,cy2,WHITE);
  gfx->fillRect(329,cy2-20,7,40,WHITE);

  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6, SCR_H-14); gfx->print("< face");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Page switcher
// ─────────────────────────────────────────────────────────────────────────────
void showPage(Page p) {
  page = p;
  if (p == PAGE_FACE) {
    dirtyFull  = true;
    dirtyEyes  = true;
    dirtyMouth = true;
    dirtyCheeks= true;
  }
  switch (p) {
    case PAGE_CLOCK:   drawClockPage();   break;
    case PAGE_WEATHER: drawWeatherPage(); break;
    case PAGE_MUSIC:   drawMusicPage();   break;
    default: break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BLE HID
// ─────────────────────────────────────────────────────────────────────────────
class DimoBLE : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConn=true;  if(page==PAGE_MUSIC) drawMusicPage(); }
  void onDisconnect(BLEServer*) override {
    bleConn=false; BLEDevice::startAdvertising();
    if(page==PAGE_MUSIC) drawMusicPage();
  }
};
void sendKey(uint8_t k) {
  if (!bleIn||!bleConn) return;
  bleIn->setValue(&k,1); bleIn->notify();
  uint8_t r=0; bleIn->setValue(&r,1); bleIn->notify();
}
void setupBLE() {
  BLEDevice::init("DIMO Remote");
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new DimoBLE());
  bleHID = new BLEHIDDevice(srv);
  bleIn  = bleHID->inputReport(1);
  bleHID->manufacturer()->setValue("DIMO");
  bleHID->pnp(0x02,0x045E,0x0000,0x0110);
  bleHID->hidInfo(0x00,0x01);
  const uint8_t rm[]={0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x01,
                      0x15,0x00,0x26,0xFF,0x00,0x75,0x08,0x95,
                      0x01,0x09,0x00,0x81,0x00,0xC0};
  bleHID->reportMap((uint8_t*)rm,sizeof(rm));
  bleHID->startServices();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);
  adv->addServiceUUID(bleHID->hidService()->getUUID());
  adv->start();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch + Swipe
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch() {
  int16_t tx, ty;
  bool pressed = touchRead(tx, ty);

  if (pressed) {
    USBSerial.printf("touch x=%d y=%d\n", tx, ty); // debug
    if (!tdDown) {
      tdDown=true; tdSX=tx; tdSY=ty;
      tdLX=tx;    tdLY=ty;
      tdMs=millis(); tdSwiped=false;
    } else {
      tdLX=tx; tdLY=ty;
    }

    if (!tdSwiped) {
      int16_t dx = tdLX - tdSX;
      int16_t dy = tdLY - tdSY;
      if (abs(dx) > 40 && abs(dx) > abs(dy)) {
        tdSwiped = true;
        if (dx < 0) showPage((Page)((page + 1) % 4));  // swipe left = next
        else        showPage((Page)((page + 3) % 4));  // swipe right = prev
      }
    }

    // Music taps
    if (page==PAGE_MUSIC && !tdSwiped && millis()-tdMs < 350) {
      if      (tx < 120)           sendKey(0xB6);
      else if (tx > SCR_W-120)     sendKey(0xB5);
      else if (abs(tx-SCR_W/2)<60) { blePlaying=!blePlaying; sendKey(0xCD); drawMusicPage(); }
    }
  } else {
    tdDown = false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  USBSerial.begin(115200);
  delay(200);

  Wire.begin(SDA_PIN, SCL_PIN);
  expander.begin(0x20);
  expander.pinMode(4,OUTPUT); expander.pinMode(5,OUTPUT);
  expander.digitalWrite(4,HIGH); expander.digitalWrite(5,HIGH);
  delay(10);

  gfx->begin();
  gfx->setBrightness(230);
  gfx->fillScreen(BLACK);

  bootAnimation();

  // WiFi connect
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info){
    if (ev==ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiOk=true;
      configTzTime(TZ_INFO,"pool.ntp.org","time.nist.gov");
    } else if (ev==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiOk=false;
    }
  });
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  wifiConnectingAnimation();

  setupBLE();

  gfx->fillScreen(BLACK);
  dirtyFull=true; dirtyEyes=true; dirtyMouth=true; dirtyCheeks=true;

  USBSerial.println("DIMO ready. Swipe left/right to switch pages.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  handleTouch();

  // ── FACE page — partial redraws only ──────────────────────────────────────
  if (page == PAGE_FACE) {

    // Full background reset only when entering face page
    if (dirtyFull) {
      dirtyFull = false;
      gfx->fillScreen(BLACK);
      dirtyEyes=true; dirtyMouth=true; dirtyCheeks=true;
    }

    // Sparkle twinkle every 600ms
    if (now - sparkTimer > 600) {
      sparkTimer = now;
      sparkFrame = (sparkFrame + 1) % 4;
      drawSparkles(sparkFrame);
    }

    // Shimmer rotate continuously (smooth eye shine)
    if (now - lastShimmer > 80) {
      lastShimmer = now;
      shimmerAngle += 0.12f;
      if (shimmerAngle > 2*PI) shimmerAngle -= 2*PI;
      dirtyEyes = true;
    }

    // Blink
    if (!isBlinking && now - blinkTimer > nextBlink) {
      isBlinking = true;
      blinkTimer = now;
      nextBlink  = random(3500, 7000);
    }
    if (isBlinking) {
      uint32_t elapsed = now - blinkTimer;
      if      (elapsed < 80)  blinkPhase = elapsed / 80.0f;
      else if (elapsed < 160) blinkPhase = 1.0f;
      else if (elapsed < 260) blinkPhase = 1.0f - (elapsed-160)/100.0f;
      else { isBlinking=false; blinkPhase=0.0f; blinkTimer=now; }
      dirtyEyes = true;
    }

    // Eye wander
    if (now - eyeMoveTimer > nextEyeMove) {
      eyeMoveTimer = now;
      nextEyeMove  = random(2500, 6000);
      eyeOX = random(-8, 9);
      eyeOY = random(-5, 6);
      dirtyEyes = true;
    }

    // Partial redraws
    if (dirtyEyes) {
      dirtyEyes = false;
      float open = 1.0f - blinkPhase;
      drawEye(EL_CX, E_CY, open, eyeOX, eyeOY);
      drawEye(ER_CX, E_CY, open, eyeOX, eyeOY);
    }
    if (dirtyMouth)  { dirtyMouth=false;  drawMouth(); }
    if (dirtyCheeks) { dirtyCheeks=false; drawCheeks(); }

    // Status bar — once per second
    static uint32_t lastStat = 0;
    if (now - lastStat > 1000) { lastStat=now; drawStatusBar(); }
  }

  // ── Clock — refresh every second ─────────────────────────────────────────
  static uint32_t lastClk = 0;
  if (page==PAGE_CLOCK && now-lastClk > 1000) { lastClk=now; drawClockPage(); }

  // ── WiFi reconnect ────────────────────────────────────────────────────────
  static uint32_t lastWifiTry = 0;
  if (!wifiOk && now-lastWifiTry > 12000) {
    lastWifiTry=now; WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  // ── Weather fetch ─────────────────────────────────────────────────────────
  if (wifiOk && !wxFetched) {
    wxFetched=true; lastWx=now;
    HTTPClient http; http.setTimeout(5000);
    if (http.begin(OWM_URL) && http.GET()==200) {
      String b=http.getString();
      float tmp=jsonF(b,"temp");
      String dsc=jsonS(b,"description");
      if (!isnan(tmp) && dsc.length()>0) {
        wxTemp=String((int)round(tmp)); wxDesc=dsc; wxOk=true;
      }
    }
    http.end();
    if (page==PAGE_WEATHER) drawWeatherPage();
    else drawStatusBar();
  }
  if (wifiOk && now-lastWx > 600000) { lastWx=now; wxFetched=false; }

  delay(16);
}

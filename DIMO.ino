/*
 * DIMO — Desktop Robot Companion
 * Waveshare ESP32-C6 Touch AMOLED 1.8" (368x448)
 *
 * FLICKER-FREE APPROACH:
 * - Eyes drawn ONCE, never fully redrawn unless page changes
 * - Blink = black rect overlay drawn then removed (3 frames, no clear)
 * - Sparkles = small pixels only, no large clears
 * - Status bar = 1 second updates only
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
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
#if __has_include("secrets.h")
#include "secrets.h"
#endif
// Font files must come after Arduino_GFX_Library.h (need GFXfont/GFXglyph types)
#include "FreeSans9pt7b.h"
#include "FreeSans12pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSansBold18pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "DimoTalkFrames.h"
// Font helpers — always use size 1 with custom fonts, never setTextSize(n>1)
#define F9   (&FreeSans9pt7b)
#define F12  (&FreeSans12pt7b)
#define FB12 (&FreeSansBold12pt7b)
#define FB18 (&FreeSansBold18pt7b)
#define FB24 (&FreeSansBold24pt7b)
#define FDEF nullptr   // default 5x7 font (for status bar tiny text only)

HWCDC USBSerial;

// ── Version ──────────────────────────────────────────────────────────────────
#define DIMO_VERSION "0.19.0-alpha.3"
#define DIMO_VERSION_NAME "Talking frame demo"

// ── Hardware ──────────────────────────────────────────────────────────────────
#define SDA_PIN  8
#define SCL_PIN  7
#define SCR_W    368
#define SCR_H    448

Adafruit_XCA9554 expander;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(5,0,1,2,3,4);
Arduino_SH8601  *gfx = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, SCR_W, SCR_H);

// ── Credentials ───────────────────────────────────────────────────────────────
#ifndef DIMO_DEFAULT_WIFI_SSID
#define DIMO_DEFAULT_WIFI_SSID ""
#endif
#ifndef DIMO_DEFAULT_WIFI_PASS
#define DIMO_DEFAULT_WIFI_PASS ""
#endif
#ifndef DIMO_OPENWEATHER_URL
#define DIMO_OPENWEATHER_URL "http://api.openweathermap.org/data/2.5/weather?q=Cumming,GA,US&units=imperial&appid=PUT_YOUR_OPENWEATHER_KEY_HERE"
#endif

char wifiSSID[64] = DIMO_DEFAULT_WIFI_SSID;        // overwritten from NVS on boot
char wifiPass[64] = DIMO_DEFAULT_WIFI_PASS;
const char *OWM_URL = DIMO_OPENWEATHER_URL;
const char *TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";

// ── WiFi portal ───────────────────────────────────────────────────────────────
Preferences  prefs;
WebServer    portalServer(80);
bool         portalActive = false;

// ── Colors ────────────────────────────────────────────────────────────────────
#define BLACK     0x0000
#define WHITE     0xFFFF
#define PAPER     0xEFFF   // warm white for the Taby-like sketch style
#define INK_DIM   0x8C71
#define BLUE_DIM  0x0256
#define ACCENT    0xF900
#define PURPLE    0xA37F
#define IRIS_COL  0x1904   // deep dark blue iris
#define PUPIL_COL 0x0000
#define SHINE_COL 0xFFFF
#define SHINE2    0xCF1B   // secondary shine (cool tint)
#define LASH_COL  0x3186   // eyelash dark
#define CHEEK_OUT 0xE00C   // outer cheek (soft pink)
#define CHEEK_MID 0xF853   // mid cheek
#define CHEEK_IN  0xFCB6   // inner cheek (bright pink)
#define SMILE_COL 0x630C   // mouth dark warm
#define DIM       0x2965
#define GRAY      0x6B6D
#define GREEN     0x07E0
#define RED_COL   0xF800
#define YELLOW    0xFFE0
#define CYAN      0x07FF
#define ORANGE    0xFD20

// ── Face layout ───────────────────────────────────────────────────────────────
#define EL_X   122    // left eye centre X
#define ER_X   246    // right eye centre X
#define EY_Y   196    // eye centre Y
#define EYE_R  44     // eye radius

#define CK_LX  76     // cheek X
#define CK_RX  292
#define CK_Y   254
#define MX     184    // mouth X
#define MY     316    // mouth Y
#define SBAR   30     // status bar height

// ── Pages ─────────────────────────────────────────────────────────────────────
enum Page { PAGE_FACE, PAGE_HOME, PAGE_CLOCK, PAGE_WEATHER, PAGE_MUSIC, PAGE_SETTINGS };
Page page = PAGE_FACE;

// ── Blink state (overlay only — no redraw) ───────────────────────────────────
enum BlinkState { BLINK_OPEN, BLINK_CLOSING, BLINK_CLOSED, BLINK_OPENING };
BlinkState blinkState = BLINK_OPEN;
uint32_t   blinkTimer = 0;
uint32_t   nextBlink  = 5000;

// ── Gaze (left/right eye animation) ──────────────────────────────────────────
int8_t   gazeDir   = 0;      // -12 = left, 0 = center, 12 = right
uint32_t gazeTimer = 0;
uint32_t nextGaze  = 4000;

// ── WiFi / weather ────────────────────────────────────────────────────────────
bool     wifiOk         = false;
bool     wifiConnecting = false;
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

// ── Digital sketch idle animation ────────────────────────────────────────────
enum FaceScene { SCENE_IDLE, SCENE_DRAWING, SCENE_MAKER, SCENE_FLOWER, SCENE_LIST };
FaceScene faceScene = SCENE_IDLE;
uint32_t  faceSceneStart = 0;
uint32_t  faceFrameTimer = 0;
uint8_t   faceFrame = 0;
uint8_t   idleMood = 0;
uint8_t   talkFrameIndex = 0;
int16_t   lastTalkFrameDrawn = -1;
uint32_t  talkFrameTimer = 0;

// ── BLE ───────────────────────────────────────────────────────────────────────
bool              bleConn    = false;
bool              bleEnabled = true;
BLEHIDDevice     *bleHID    = nullptr;
BLECharacteristic*bleIn     = nullptr;
bool              blePlaying = false;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
// Print centred on x-axis. With custom fonts y = baseline; with default font y = top.
void centered(const char *s, int16_t y, uint16_t col,
              const GFXfont *font = FDEF, uint8_t sz = 1) {
  gfx->setFont(font); gfx->setTextSize(sz); gfx->setTextColor(col);
  int16_t x1,y1; uint16_t w,h;
  gfx->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  gfx->setCursor((SCR_W - (int16_t)w) / 2, y);
  gfx->print(s);
  gfx->setFont(FDEF); // always reset
}

void thickLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t w, uint16_t col) {
  for (int8_t o = -(int8_t)w/2; o <= (int8_t)w/2; o++) {
    gfx->drawLine(x1 + o, y1, x2 + o, y2, col);
    gfx->drawLine(x1, y1 + o, x2, y2 + o, col);
  }
}

void thickArc(int16_t cx, int16_t cy, int16_t rx, int16_t ry,
              int16_t startDeg, int16_t endDeg, uint8_t w, uint16_t col) {
  int16_t px = cx + (int16_t)(cosf(startDeg * PI / 180.0f) * rx);
  int16_t py = cy + (int16_t)(sinf(startDeg * PI / 180.0f) * ry);
  for (int16_t a = startDeg + 4; a <= endDeg; a += 4) {
    float r = a * PI / 180.0f;
    int16_t x = cx + (int16_t)(cosf(r) * rx);
    int16_t y = cy + (int16_t)(sinf(r) * ry);
    thickLine(px, py, x, y, w, col);
    px = x; py = y;
  }
}

void drawSparkle(int16_t x, int16_t y, uint8_t s, uint16_t col) {
  thickLine(x-s, y, x+s, y, 1, col);
  thickLine(x, y-s, x, y+s, 1, col);
  gfx->drawPixel(x-s-2, y-s-2, col);
  gfx->drawPixel(x+s+2, y+s+2, col);
}

void drawCapsuleEye(int16_t x, int16_t y, int16_t w, int16_t h, int8_t lift = 0) {
  gfx->fillRoundRect(x - w/2, y - h/2 + lift, w, h, h/2, PAPER);
}

void drawTinySmile(int16_t cx, int16_t cy, uint8_t width, uint16_t col = PAPER) {
  thickArc(cx, cy - 8, width, 18, 28, 152, 3, col);
}

void drawDigitalFaceBase(int8_t mood = 0) {
  int16_t bob = (faceFrame % 16 < 8) ? -2 : 0;
  int16_t eyeY = 190 + bob;

  if (mood == 2) {
    gfx->fillCircle(118, eyeY, 22, PAPER);
    gfx->fillCircle(118, eyeY, 8, BLACK);
    gfx->fillCircle(250, eyeY, 22, PAPER);
    gfx->fillCircle(250, eyeY, 8, BLACK);
    thickArc(118, eyeY + 2, 34, 26, 205, 335, 8, BLACK);
    thickArc(250, eyeY + 2, 34, 26, 205, 335, 8, BLACK);
  } else {
    drawCapsuleEye(120, eyeY, 44, mood == 1 ? 26 : 34, mood == 1 ? 8 : 0);
    drawCapsuleEye(248, eyeY, 44, mood == 1 ? 26 : 34, mood == 1 ? 8 : 0);
  }

  if (mood == 1) {
    thickLine(92, eyeY - 52, 128, eyeY - 62, 5, PAPER);
    thickLine(240, eyeY - 62, 276, eyeY - 52, 5, PAPER);
    thickLine(170, 268, 198, 268, 4, PAPER);
  } else {
    thickArc(118, eyeY - 58, 24, 13, 205, 335, 5, PAPER);
    thickArc(250, eyeY - 58, 24, 13, 205, 335, 5, PAPER);
    drawTinySmile(184, 260, mood == 2 ? 44 : 32);
  }
}
float  jsonF(const String &b, const char *k) {
  String key=String("\"")+k+"\":"; int i=b.indexOf(key);
  if(i<0) return NAN;
  return b.substring(i+key.length(),i+key.length()+10).toFloat();
}
String jsonS(const String &b, const char *k) {
  String key=String("\"")+k+"\":\""; int i=b.indexOf(key);
  if(i<0) return "";
  int s=i+key.length(), e=b.indexOf('"',s);
  return b.substring(s,e);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch (FT3168 I2C 0x38)
// ─────────────────────────────────────────────────────────────────────────────
bool touchRead(int16_t &x, int16_t &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false)!=0) return false;
  Wire.requestFrom(0x38,6);
  if (Wire.available()<6) return false;
  uint8_t td=Wire.read(),xh=Wire.read(),xl=Wire.read(),
              yh=Wire.read(),yl=Wire.read();
  Wire.read();
  if ((td&0x0F)==0) return false;
  // 0x00=press, 0x80=contact/move — both valid. Reject 0x40=lift, 0xC0=no event
  uint8_t evt = xh & 0xC0;
  if (evt == 0x40 || evt == 0xC0) return false;
  x=((xh&0x0F)<<8)|xl;
  y=((yh&0x0F)<<8)|yl;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Digital sketch face + idle scenes
// ─────────────────────────────────────────────────────────────────────────────
void drawEyeFull(int16_t cx, int16_t cy, int8_t gaze = 0) {
  drawCapsuleEye(cx + gaze, cy, 44, 34);
}

void blinkOverlay(int16_t cx, int16_t cy, int step) {
  if (step == 1) {
    gfx->fillRoundRect(cx - 24, cy - 18, 48, 28, 10, BLACK);
    gfx->fillRoundRect(cx - 21, cy - 2, 42, 5, 2, PAPER);
  } else if (step == 2) {
    gfx->fillRoundRect(cx - 24, cy - 20, 48, 40, 10, BLACK);
    gfx->fillRoundRect(cx - 19, cy - 2, 38, 5, 2, PAPER);
  } else {
    drawEyeFull(cx, cy, gazeDir);
  }
}

void drawCheeks() {
  drawSparkle(74, 128, 9, INK_DIM);
  drawSparkle(292, 128, 7, INK_DIM);
}

void drawMouth() {
  drawTinySmile(MX, 258, 32);
}

void drawEyebrows() {
  thickArc(118, 132, 24, 13, 205, 335, 5, PAPER);
  thickArc(250, 132, 24, 13, 205, 335, 5, PAPER);
}

void drawPaperSheet(int16_t x, int16_t y, int16_t w, int16_t h, int16_t tilt) {
  thickLine(x, y + h, x + w, y + h - tilt, 2, PAPER);
  thickLine(x, y + h, x + 12, y, 2, PAPER);
  thickLine(x + 12, y, x + w + 12, y - tilt, 2, PAPER);
  thickLine(x + w + 12, y - tilt, x + w, y + h - tilt, 2, PAPER);
  thickLine(x + 22, y + h - 17, x + w - 18, y + h - 26 - tilt, 1, INK_DIM);
  thickLine(x + 30, y + h - 28, x + w - 8, y + h - 38 - tilt, 1, INK_DIM);
  thickLine(x + 42, y + h - 39, x + w - 32, y + h - 48 - tilt, 1, INK_DIM);
}

void drawPencil(int16_t x, int16_t y, int16_t a) {
  thickLine(x, y, x + 72, y + a, 5, PAPER);
  thickLine(x + 68, y + a - 12, x + 90, y + a + 3, 2, PAPER);
  thickLine(x + 68, y + a + 12, x + 90, y + a + 3, 2, PAPER);
  thickLine(x + 14, y + 2, x + 24, y + a + 10, 1, BLACK);
  gfx->fillTriangle(x + 86, y + a - 2, x + 98, y + a + 4, x + 86, y + a + 10, PAPER);
}

void drawChecklistIcon(int16_t cx, int16_t cy) {
  gfx->drawRoundRect(cx - 48, cy - 62, 96, 124, 8, PAPER);
  gfx->drawRoundRect(cx - 19, cy - 48, 38, 10, 4, PAPER);
  for (int i = 0; i < 3; i++) {
    int16_t yy = cy - 18 + i * 28;
    gfx->drawRoundRect(cx - 36, yy - 8, 12, 12, 3, PAPER);
    thickLine(cx - 12, yy - 2, cx + 32, yy - 2, 3, PAPER);
  }
}

void drawMakerProp(int16_t baseY) {
  thickLine(32, baseY + 44, 224, baseY - 20, 2, INK_DIM);
  thickLine(54, baseY + 38, 244, baseY + 4, 2, INK_DIM);
  thickLine(74, baseY + 28, 136, baseY - 48, 2, INK_DIM);
  thickLine(142, baseY + 16, 200, baseY - 46, 2, INK_DIM);
  gfx->drawCircle(104, baseY - 2, 28, INK_DIM);
  gfx->drawRoundRect(166, baseY - 30, 56, 44, 3, INK_DIM);
  gfx->drawRoundRect(204, baseY - 54, 14, 56, 7, PAPER);
  gfx->fillRoundRect(196, baseY - 2, 30, 8, 4, PAPER);
  thickLine(260, baseY + 20, 272, baseY + 8, 4, ACCENT);
  thickLine(260, baseY + 8, 272, baseY + 20, 4, ACCENT);
}

void drawFlowerEye(int16_t cx, int16_t cy) {
  for (int a = 0; a < 360; a += 72) {
    float r = a * PI / 180.0f;
    gfx->fillEllipse(cx + (int16_t)(cosf(r) * 17), cy + (int16_t)(sinf(r) * 17), 15, 19, PAPER);
  }
  gfx->fillCircle(cx, cy, 13, PAPER);
}

void drawFaceScene() {
  gfx->fillRect(48, 86, 272, 238, BLACK);

  if (idleMood == 2) {
    thickArc(120 + gazeDir / 2, 190, 30, 18, 20, 160, 5, PAPER);
    thickArc(248 + gazeDir / 2, 190, 30, 18, 20, 160, 5, PAPER);
    thickLine(96, 132, 134, 124, 5, PAPER);
    thickLine(234, 124, 272, 132, 5, PAPER);
    gfx->drawCircle(184, 258, 12, PAPER);
    gfx->drawCircle(184, 258, 13, PAPER);
  } else {
    int16_t ly = idleMood == 1 ? 184 : 190;
    int16_t ry = idleMood == 1 ? 196 : 190;
    drawCapsuleEye(120 + gazeDir, ly, 50, idleMood == 1 ? 28 : 36);
    drawCapsuleEye(248 + gazeDir, ry, 50, idleMood == 1 ? 36 : 36);

    if (idleMood == 1) {
      thickLine(94, 132, 132, 124, 5, PAPER);
      thickArc(248, 134, 24, 12, 205, 335, 5, PAPER);
      drawTinySmile(184, 260, 20);
    } else {
      thickArc(120, 132, 24, 12, 205, 335, 5, PAPER);
      thickArc(248, 132, 24, 12, 205, 335, 5, PAPER);
      drawTinySmile(184, 260, 34);
    }
  }

  drawSparkle(78, 126, 5, INK_DIM);
  drawSparkle(292, 126, 4, INK_DIM);
}

void drawTalkRuns(uint8_t frame, uint16_t colorOverride = 0xFFFF, bool overrideColor = false) {
  const TalkRun *runs = (const TalkRun *)pgm_read_ptr(&talkFrames[frame]);
  uint16_t count = pgm_read_word(&talkFrameRunCounts[frame]);
  for (uint16_t i = 0; i < count; i++) {
    TalkRun r;
    memcpy_P(&r, &runs[i], sizeof(TalkRun));
    uint16_t col = overrideColor ? colorOverride : TALK_COLORS[r.color];
    gfx->drawFastHLine(r.x, r.y, r.len, col);
  }
}

void drawTalkFrame(uint8_t frame) {
  if (lastTalkFrameDrawn >= 0) {
    drawTalkRuns((uint8_t)lastTalkFrameDrawn, BLACK, true);
  }
  drawTalkRuns(frame);
  lastTalkFrameDrawn = frame;
}

void drawFacePage() {
  faceScene = SCENE_IDLE;
  faceSceneStart = millis();
  faceFrameTimer = 0;
  faceFrame = 0;
  idleMood = 0;
  gazeDir = 0;
  talkFrameIndex = 0;
  lastTalkFrameDrawn = -1;
  talkFrameTimer = millis();
  gfx->fillScreen(BLACK);
  drawTalkFrame(talkFrameIndex);
}

void updateGaze(int8_t newDir) {
  gazeDir = newDir;
  drawFaceScene();
}

// ─────────────────────────────────────────────────────────────────────────────
//  STATUS BAR
// ─────────────────────────────────────────────────────────────────────────────
void drawStatusBar() {
  gfx->fillRect(0, 0, SCR_W, SBAR, BLACK);
  struct tm t;
  if (getLocalTime(&t,30)) {
    char buf[8];
    snprintf(buf,sizeof(buf),"%02d:%02d",t.tm_hour,t.tm_min);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(8,11); gfx->print(buf);
  }
  gfx->fillCircle(SCR_W/2, 15, 4, wifiOk ? GREEN : RED_COL);
  if (wxOk) {
    String s=wxTemp+"F";
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(SCR_W-(int16_t)s.length()*6-8, 11);
    gfx->print(s);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BOOT ANIMATION
// ─────────────────────────────────────────────────────────────────────────────
void bootAnimation() {
  gfx->fillScreen(BLACK);

  // 1. Tiny dots blink on
  for (int r=2; r<=10; r+=2) {
    gfx->fillCircle(EL_X, EY_Y, r, r>6?0x0529:0x0208);
    gfx->fillCircle(ER_X, EY_Y, r, r>6?0x0529:0x0208);
    delay(50);
  }
  delay(300);

  // 2. Eyes grow open
  for (int r=8; r<=EYE_R; r+=4) {
    gfx->fillCircle(EL_X, EY_Y, r+2, BLACK);
    gfx->fillCircle(ER_X, EY_Y, r+2, BLACK);
    gfx->fillCircle(EL_X, EY_Y, r,   WHITE);
    gfx->fillCircle(EL_X, EY_Y, max(0,r-14), IRIS_COL);
    gfx->fillCircle(EL_X, EY_Y, max(0,r-22), BLACK);
    gfx->fillCircle(ER_X, EY_Y, r,   WHITE);
    gfx->fillCircle(ER_X, EY_Y, max(0,r-14), IRIS_COL);
    gfx->fillCircle(ER_X, EY_Y, max(0,r-22), BLACK);
    delay(30);
  }
  delay(200);

  // 3. Full cute face
  drawFacePage();
  delay(500);

  // 4. DIMO name
  gfx->fillScreen(BLACK);
  gfx->setFont(FB24); gfx->setTextColor(WHITE); gfx->setTextSize(1);
  int16_t bx1,by1; uint16_t bw,bh;
  gfx->getTextBounds("DIMO", 0, SCR_H/2-20, &bx1,&by1,&bw,&bh);
  gfx->setCursor((SCR_W-(int16_t)bw)/2, SCR_H/2-20);
  gfx->print("DIMO");
  gfx->setFont(FDEF);
  centered("your desktop friend", SCR_H/2+32, DIM, F9);
  delay(900);

  // 5. Ready — face with sparkle burst
  drawFacePage();
  delay(600);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WIFI ANIMATION
// ─────────────────────────────────────────────────────────────────────────────
void wifiAnimation() {
  auto wifiIcon = [](int bars, uint16_t col){
    int16_t cx=SCR_W/2, cy=SCR_H-80;
    gfx->fillCircle(cx,cy,5,col);
    if(bars>=1){gfx->drawCircle(cx,cy,16,col);gfx->drawCircle(cx,cy,17,col);}
    if(bars>=2){gfx->drawCircle(cx,cy,28,col);gfx->drawCircle(cx,cy,29,col);}
    if(bars>=3){gfx->drawCircle(cx,cy,40,col);gfx->drawCircle(cx,cy,41,col);}
    gfx->fillRect(cx-44,cy-44,88,44,BLACK);
  };

  // Looking left
  gfx->fillScreen(BLACK);
  gfx->fillCircle(EL_X,EY_Y,EYE_R,WHITE); gfx->fillCircle(EL_X+16,EY_Y,EYE_R-2,BLACK);
  gfx->fillCircle(ER_X,EY_Y,EYE_R,WHITE); gfx->fillCircle(ER_X+16,EY_Y,EYE_R-2,BLACK);
  delay(400);

  // Looking right + 1 bar
  gfx->fillScreen(BLACK);
  gfx->fillCircle(EL_X,EY_Y,EYE_R,WHITE); gfx->fillCircle(EL_X-16,EY_Y,EYE_R-2,BLACK);
  gfx->fillCircle(ER_X,EY_Y,EYE_R,WHITE); gfx->fillCircle(ER_X-16,EY_Y,EYE_R-2,BLACK);
  wifiIcon(1,DIM);
  delay(400);

  // Centre + 2 bars
  gfx->fillScreen(BLACK);
  drawEyeFull(EL_X,EY_Y); drawEyeFull(ER_X,EY_Y);
  wifiIcon(2,CYAN);
  delay(400);

  // Eyes up + 3 bars
  gfx->fillScreen(BLACK);
  drawEyeFull(EL_X,EY_Y-10); drawEyeFull(ER_X,EY_Y-10);
  wifiIcon(3,CYAN);
  delay(400);

  // Connected — full face
  drawFacePage();
  centered("Connected!", SCR_H-40, GREEN, FB12);
  delay(700);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLOCK PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawClockPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  struct tm t;
  if (!getLocalTime(&t,200)) {
    centered("Syncing...", SCR_H/2, DIM, FB18);
    gfx->setTextColor(DIM); gfx->setTextSize(1);
    gfx->setCursor(6,SCR_H-14); gfx->print("< face");
    gfx->setCursor(SCR_W-76,SCR_H-14); gfx->print("weather >");
    return;
  }

  char tBuf[8],dBuf[16];
  snprintf(tBuf,sizeof(tBuf),"%02d:%02d",t.tm_hour,t.tm_min);
  strftime(dBuf,sizeof(dBuf),"%a  %b %d",&t);

  // Sleeping mini eyes
  int16_t fy=100;
  gfx->fillCircle(EL_X,fy,20,WHITE); gfx->fillRect(EL_X-22,fy-22,44,22,BLACK);
  gfx->fillCircle(EL_X,fy+5,11,IRIS_COL);
  gfx->fillCircle(ER_X,fy,20,WHITE); gfx->fillRect(ER_X-22,fy-22,44,22,BLACK);
  gfx->fillCircle(ER_X,fy+5,11,IRIS_COL);
  gfx->fillRoundRect(MX-14,fy+28,28,5,2,DIM);
  gfx->setTextColor(DIM);gfx->setTextSize(1);
  gfx->setCursor(ER_X+22,fy-24);gfx->print("z");
  gfx->setTextSize(2);gfx->setCursor(ER_X+36,fy-44);gfx->print("Z");
  gfx->setTextSize(3);gfx->setCursor(ER_X+54,fy-70);gfx->print("Z");

  // Big time
  centered(tBuf, 210, WHITE, FB24);
  centered(dBuf, 248, CYAN, F12);

  // Seconds ring
  gfx->drawCircle(SCR_W/2,336,48,DIM);
  float ang=(t.tm_sec/60.0f)*2*PI-PI/2;
  gfx->fillCircle(SCR_W/2+(int)(48*cos(ang)),336+(int)(48*sin(ang)),7,CYAN);

  gfx->setTextColor(DIM);gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-14);gfx->print("< face");
  gfx->setCursor(SCR_W-76,SCR_H-14);gfx->print("weather >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  DEAD FACE — shown when no WiFi
// ─────────────────────────────────────────────────────────────────────────────
void drawDeadFace() {
  gfx->fillScreen(BLACK);

  // Crack lines radiating from face center
  uint16_t cc = 0x2965;
  int16_t fx = SCR_W/2, fy = SCR_H/2 - 10;
  gfx->drawLine(fx,    fy,    fx-28, fy-55, cc); gfx->drawLine(fx-28, fy-55, fx-70, fy-44, cc); gfx->drawLine(fx-70, fy-44, fx-110, fy-88, cc);
  gfx->drawLine(fx,    fy,    fx+12, fy-62, cc); gfx->drawLine(fx+12, fy-62, fx+44, fy-48, cc); gfx->drawLine(fx+44, fy-48, fx+32,  fy-110,cc);
  gfx->drawLine(fx,    fy,    fx+66, fy-28, cc); gfx->drawLine(fx+66, fy-28, fx+84, fy-64, cc);
  gfx->drawLine(fx,    fy,    fx+88, fy+18, cc); gfx->drawLine(fx+88, fy+18, fx+110,fy+64, cc); gfx->drawLine(fx+110,fy+64, fx+140,fy+88, cc);
  gfx->drawLine(fx,    fy,    fx-18, fy+84, cc); gfx->drawLine(fx-18, fy+84, fx+4,  fy+130,cc);
  gfx->drawLine(fx,    fy,    fx-82, fy+28, cc); gfx->drawLine(fx-82, fy+28, fx-120,fy+62, cc); gfx->drawLine(fx-120,fy+62, fx-138,fy+108,cc);
  gfx->drawLine(fx,    fy,    fx-58, fy-18, cc); gfx->drawLine(fx-58, fy-18, fx-88, fy+22, cc);

  // Subtle glow behind eyes
  gfx->fillCircle(EL_X, EY_Y, 32, 0x0208);
  gfx->fillCircle(ER_X, EY_Y, 36, 0x0208);

  // Furrowed eyebrows (angry/dead)
  int16_t bby = EY_Y - EYE_R - 14;
  for (int t=0; t<4; t++) {
    gfx->drawLine(EL_X-24, bby-8+t, EL_X+20, bby+6+t, DIM);  // left: outer-high, inner-low
    gfx->drawLine(ER_X-20, bby+6+t, ER_X+24, bby-8+t, DIM);  // right: inner-low, outer-high
  }

  // ∪ left eye (closed droopy arc)
  const int16_t uer=24, udepth=18;
  for (int x=-uer; x<=uer; x++) {
    int16_t dy = udepth - (int16_t)((float)x*x*udepth/(uer*uer));
    int16_t py = EY_Y - udepth/2 + dy;
    for (int t=0; t<5; t++) {
      uint16_t col = (t==2) ? 0x07FF : 0x05BF;
      gfx->drawPixel(EL_X+x, py+t, col);
    }
  }

  // X right eye
  int16_t xr = 24;
  for (int t=-3; t<=3; t++) {
    gfx->drawLine(ER_X-xr+t, EY_Y-xr, ER_X+xr+t, EY_Y+xr, 0x05BF);
    gfx->drawLine(ER_X-xr,   EY_Y-xr+t, ER_X+xr, EY_Y+xr+t, 0x05BF);
    gfx->drawLine(ER_X-xr+t, EY_Y+xr, ER_X+xr+t, EY_Y-xr, 0x05BF);
    gfx->drawLine(ER_X-xr,   EY_Y+xr+t, ER_X+xr, EY_Y-xr+t, 0x05BF);
  }
  // Bright center pixels on X
  gfx->drawLine(ER_X-xr, EY_Y-xr, ER_X+xr, EY_Y+xr, 0x07FF);
  gfx->drawLine(ER_X-xr, EY_Y+xr, ER_X+xr, EY_Y-xr, 0x07FF);

  // ∩ frown mouth
  const int16_t fr=28, fdepth=14;
  for (int x=-fr; x<=fr; x++) {
    int16_t dy = (int16_t)((float)x*x*fdepth/(fr*fr));
    int16_t py = MY - fdepth + dy; // center HIGH, edges LOW = ∩
    for (int t=0; t<5; t++) {
      uint16_t col = (t==2) ? 0x07FF : 0x05BF;
      gfx->drawPixel(MX+x, py+t, col);
    }
  }

  centered("No WiFi", MY+64, DIM, F12);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WEATHER PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  if (!wifiOk) { drawDeadFace(); return; }

  gfx->fillScreen(BLACK);
  drawStatusBar();
  centered("Cumming, GA", 56, DIM, F9);

  if (!wxOk)   { centered("Loading...", SCR_H/2, DIM, FB18); goto hints; }

  {
    // Big temp
    String ts = wxTemp + "°F";
    centered(ts.c_str(), 148, WHITE, FB24);

    // Description
    String dc = wxDesc; if (dc.length()>0) dc[0]=toupper(dc[0]);
    centered(dc.c_str(), 178, CYAN, F12);

    // Icon
    int16_t ix=SCR_W/2,iy=278;
    if(wxDesc.indexOf("clear")>=0||wxDesc.indexOf("sun")>=0){
      gfx->fillCircle(ix,iy,44,YELLOW);
      for(int a=0;a<360;a+=45){
        float r=a*PI/180;
        gfx->drawLine(ix+(int)(54*cos(r)),iy+(int)(54*sin(r)),
                      ix+(int)(70*cos(r)),iy+(int)(70*sin(r)),YELLOW);
        gfx->drawLine(ix+(int)(55*cos(r)),iy+(int)(55*sin(r)),
                      ix+(int)(69*cos(r)),iy+(int)(69*sin(r)),YELLOW);
      }
    } else if(wxDesc.indexOf("rain")>=0||wxDesc.indexOf("drizzle")>=0||wxDesc.indexOf("storm")>=0){
      gfx->fillEllipse(ix,iy-10,52,30,GRAY);
      gfx->fillEllipse(ix-22,iy-24,32,22,GRAY);
      gfx->fillEllipse(ix+18,iy-26,26,18,GRAY);
      for(int d=-2;d<=2;d++) gfx->fillRoundRect(ix+d*20-3,iy+26,6,22,3,CYAN);
    } else {
      gfx->fillEllipse(ix,iy,52,30,GRAY);
      gfx->fillEllipse(ix-22,iy-16,32,22,GRAY);
      gfx->fillEllipse(ix+18,iy-18,26,18,GRAY);
    }
  }

  hints:
  gfx->setTextColor(DIM);gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-14);gfx->print("< clock");
  gfx->setCursor(SCR_W-52,SCR_H-14);gfx->print("face >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  MUSIC PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawMusicPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  // Small eyes
  int16_t fy=130;
  gfx->fillCircle(EL_X,fy,22,WHITE);gfx->fillCircle(EL_X,fy,14,IRIS_COL);
  gfx->fillCircle(EL_X,fy,6,BLACK);gfx->fillCircle(EL_X+7,fy-6,5,WHITE);
  gfx->fillCircle(ER_X,fy,22,WHITE);gfx->fillCircle(ER_X,fy,14,IRIS_COL);
  gfx->fillCircle(ER_X,fy,6,BLACK);gfx->fillCircle(ER_X+7,fy-6,5,WHITE);
  for(int x=-18;x<=18;x++){
    int16_t y=(int16_t)((float)x*x*7/(18*18));
    gfx->drawPixel(MX+x,fy+34+y,SMILE_COL);
    gfx->drawPixel(MX+x,fy+35+y,SMILE_COL);
  }

  if(bleConn) centered("Connected", 196, GREEN, F12);
  else        centered("Pair: DIMO Remote", 196, DIM, F9);

  // Audio bars
  int16_t by=304;
  uint8_t bh[]={18,32,50,62,50,32,18};
  for(int i=0;i<7;i++){
    uint16_t bc=(i==3)?CYAN:(i==2||i==4)?0x03DF:DIM;
    gfx->fillRoundRect(SCR_W/2-52+i*16,by-bh[i],12,bh[i],3,bc);
  }

  int16_t cy2=SCR_H-66;
  gfx->fillTriangle(56,cy2-20,56,cy2+20,34,cy2,WHITE);
  gfx->fillRect(32,cy2-20,7,40,WHITE);
  if(blePlaying){
    gfx->fillRoundRect(SCR_W/2-18,cy2-22,14,44,2,GREEN);
    gfx->fillRoundRect(SCR_W/2+4,cy2-22,14,44,2,GREEN);
  } else {
    gfx->fillTriangle(SCR_W/2-14,cy2-22,SCR_W/2-14,cy2+22,SCR_W/2+18,cy2,GREEN);
  }
  gfx->fillTriangle(312,cy2-20,312,cy2+20,334,cy2,WHITE);
  gfx->fillRect(330,cy2-20,7,40,WHITE);

  gfx->setTextColor(DIM);gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-14);gfx->print("< face");
  gfx->setCursor(SCR_W-90,SCR_H-14);gfx->print("settings >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SETTINGS PAGE
// ─────────────────────────────────────────────────────────────────────────────
#define SET_ROW_Y1  90    // WiFi row top
#define SET_ROW_Y2  200   // BT row top
#define SET_ROW_Y3  310   // WiFi Setup row top
#define SET_ROW_H   92

void drawToggle(int16_t x, int16_t y, bool on) {
  uint16_t bg = on ? 0x0400 : GRAY; // green : gray
  gfx->fillRoundRect(x, y, 58, 28, 14, bg);
  int16_t kx = on ? x+34 : x+6;
  gfx->fillCircle(kx+7, y+14, 11, WHITE);
}

void drawSettingsRow(int16_t ry, const char* label, const char* sub, bool on, uint16_t iconCol) {
  gfx->fillRoundRect(12, ry, SCR_W-24, SET_ROW_H-4, 10, 0x18C6);
  gfx->fillCircle(44, ry+42, 20, iconCol);
  gfx->setFont(FB12); gfx->setTextColor(WHITE);
  gfx->setCursor(76, ry+36); gfx->print(label);
  gfx->setFont(F9);  gfx->setTextColor(DIM);
  gfx->setCursor(76, ry+58); gfx->print(sub);
  gfx->setFont(FDEF);
  drawToggle(SCR_W-78, ry+28, on);
}

// ── WiFi portal functions ─────────────────────────────────────────────────────
void stopPortal() {
  portalActive = false;
  portalServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);
}

void startPortal() {
  if (wifiOk) { WiFi.disconnect(); wifiOk=false; }
  wifiConnecting=false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("DIMO-Setup", "dimo1234");
  delay(200);

  // Serve setup page
  portalServer.on("/", HTTP_GET, [](){
    int n = WiFi.scanNetworks();
    String opts = "";
    for (int i=0; i<n; i++)
      opts += "<option value=\""+WiFi.SSID(i)+"\">"+WiFi.SSID(i)+" ("+WiFi.RSSI(i)+"dBm)</option>";
    String html = "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>DIMO WiFi Setup</title>"
      "<style>body{font-family:sans-serif;background:#111;color:#fff;padding:24px;max-width:400px;margin:auto}"
      "h2{color:#07FF}select,input{width:100%;padding:12px;margin:8px 0;background:#222;color:#fff;"
      "border:1px solid #444;border-radius:10px;font-size:16px;box-sizing:border-box}"
      "button{width:100%;padding:14px;background:#07E0;color:#000;border:none;border-radius:10px;"
      "font-size:18px;font-weight:bold;margin-top:10px}</style></head><body>"
      "<h2>DIMO WiFi Setup</h2>"
      "<p>Select your network:</p>"
      "<select onchange=\"document.getElementById('s').value=this.value\">"
      "<option value=''>-- choose --</option>"+opts+"</select>"
      "<form method='POST' action='/save'>"
      "<input type='text' name='ssid' id='s' placeholder='Network name (SSID)' required>"
      "<input type='password' name='pass' placeholder='Password'>"
      "<button type='submit'>Connect DIMO</button>"
      "</form></body></html>";
    portalServer.send(200, "text/html", html);
  });

  portalServer.on("/save", HTTP_POST, [](){
    String ssid = portalServer.arg("ssid");
    String pass = portalServer.arg("pass");
    if (ssid.length() > 0) {
      ssid.toCharArray(wifiSSID, sizeof(wifiSSID));
      pass.toCharArray(wifiPass, sizeof(wifiPass));
      prefs.begin("dimo", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      portalServer.send(200, "text/html",
        "<html><body style='font-family:sans-serif;background:#111;color:#fff;padding:24px'>"
        "<h2 style='color:#07FF'>Saved!</h2><p>DIMO is connecting to <b>"+ssid+"</b></p>"
        "<p>You can now reconnect your phone to that network.</p></body></html>");
      delay(1000);
      stopPortal();
      wifiConnecting=true;
      WiFi.begin(wifiSSID, wifiPass);
      if (page==PAGE_SETTINGS) drawSettingsPage();
    } else {
      portalServer.send(400, "text/html", "<html><body>SSID required</body></html>");
    }
  });

  portalServer.begin();
  portalActive = true;
}

void drawPortalScreen() {
  gfx->fillScreen(BLACK);
  centered("WiFi Setup", 46, CYAN, FB18);
  gfx->drawFastHLine(20, 58, SCR_W-40, 0x2104);

  // Step 1
  gfx->fillCircle(28, 92, 16, 0x0289);
  centered("1", 98, WHITE, FB12); // approximate
  gfx->setFont(F12); gfx->setTextColor(WHITE);
  gfx->setCursor(52, 98); gfx->print("On your phone, connect");
  gfx->setCursor(52,118); gfx->print("to this WiFi:");
  gfx->setFont(FDEF);
  centered("DIMO-Setup", 148, CYAN, FB18);
  centered("Password:  dimo1234", 174, DIM, F9);

  // Step 2
  gfx->fillCircle(28, 206, 16, 0x0289);
  gfx->setFont(F12); gfx->setTextColor(WHITE);
  gfx->setCursor(52, 212); gfx->print("Open browser, go to:");
  gfx->setFont(FDEF);
  centered("192.168.4.1", 248, CYAN, FB18);

  // Step 3
  gfx->setFont(F12); gfx->setTextColor(WHITE);
  gfx->setCursor(20, 290); gfx->print("Select network & enter");
  gfx->setCursor(20, 312); gfx->print("password, tap Connect.");
  gfx->setFont(FDEF);

  // Cancel button
  gfx->fillRoundRect(40, 356, SCR_W-80, 52, 12, 0x2104);
  centered("Tap to cancel", 389, GRAY, F12);
}

void drawSettingsPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();
  centered("Settings", 56, WHITE, FB18);
  gfx->drawFastHLine(20, 68, SCR_W-40, 0x2104);

  String wfSub = wifiOk ? String("Connected: ")+wifiSSID
                        : (wifiConnecting ? "Connecting..." : "Off");
  drawSettingsRow(SET_ROW_Y1, "WiFi", wfSub.c_str(), wifiOk||wifiConnecting, 0x02DF);

  const char* btSub = bleConn ? "Device connected" : (bleEnabled ? "Visible: DIMO" : "Off");
  drawSettingsRow(SET_ROW_Y2, "Bluetooth", btSub, bleEnabled, 0x001F);

  // Change network row
  gfx->fillRoundRect(12, SET_ROW_Y3, SCR_W-24, SET_ROW_H-4, 10, 0x18C6);
  gfx->fillCircle(44, SET_ROW_Y3+42, 20, 0x0289);
  gfx->setFont(F9); gfx->setTextColor(WHITE);
  gfx->setCursor(36, SET_ROW_Y3+46); gfx->print("AP");
  gfx->setFont(FB12); gfx->setTextColor(WHITE);
  gfx->setCursor(76, SET_ROW_Y3+36); gfx->print("Change Network");
  gfx->setFont(F9); gfx->setTextColor(DIM);
  gfx->setCursor(76, SET_ROW_Y3+58);
  gfx->print(strlen(wifiSSID)>0 ? wifiSSID : "No saved network");
  gfx->setFont(FDEF);

  centered("swipe up for home", SCR_H-10, DIM);
}

void handleSettingsTap(int16_t y) {
  if (y >= SET_ROW_Y1 && y < SET_ROW_Y1+SET_ROW_H) {
    if (wifiOk)           { WiFi.disconnect(); wifiOk=false; wifiConnecting=false; wxOk=false; wxFetched=false; }
    else if (wifiConnecting) { WiFi.disconnect(); wifiConnecting=false; }
    else                  { wifiConnecting=true; WiFi.begin(wifiSSID, wifiPass); }
    drawSettingsPage();
  } else if (y >= SET_ROW_Y2 && y < SET_ROW_Y2+SET_ROW_H) {
    bleEnabled = !bleEnabled;
    if (bleEnabled) BLEDevice::startAdvertising();
    else            BLEDevice::stopAdvertising();
    drawSettingsPage();
  } else if (y >= SET_ROW_Y3) {
    startPortal();
    drawPortalScreen();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOME PAGE — app grid (Apple Watch style)
// ─────────────────────────────────────────────────────────────────────────────
#define ICO_R  40
#define ICO_Y1 170
#define ICO_Y2 310
static const int16_t ICO_X[] = {68, 184, 300};

// Base: glow ring + filled circle + label
void drawIconBase(int16_t cx, int16_t cy, uint16_t bg, const char* label) {
  gfx->fillCircle(cx, cy, ICO_R+5, 0x0841);
  gfx->fillCircle(cx, cy, ICO_R,   bg);
  gfx->setFont(F9); gfx->setTextColor(WHITE); gfx->setTextSize(1);
  int16_t x1,y1; uint16_t w,h;
  int16_t labelY = cy+ICO_R+18;
  gfx->getTextBounds(label, cx, labelY, &x1, &y1, &w, &h);
  gfx->setCursor(cx - (int16_t)w/2, labelY);
  gfx->print(label);
  gfx->setFont(FDEF);
}

void drawGearIcon(int16_t cx, int16_t cy) {
  gfx->fillCircle(cx, cy, 18, WHITE);
  gfx->fillCircle(cx, cy,  8, 0x4A49);
  for (int a=0; a<8; a++) {
    float r = a*0.785398f;
    gfx->fillCircle(cx+(int16_t)(cosf(r)*19), cy+(int16_t)(sinf(r)*19), 5, WHITE);
  }
}

void drawClockIcon(int16_t cx, int16_t cy) {
  gfx->drawCircle(cx, cy, 18, CYAN); gfx->drawCircle(cx, cy, 19, CYAN);
  gfx->drawLine(cx, cy, cx, cy-13, CYAN);
  gfx->drawLine(cx, cy, cx+9, cy+3, CYAN);
  gfx->fillCircle(cx, cy, 2, CYAN);
}

void drawSunIcon(int16_t cx, int16_t cy) {
  gfx->fillCircle(cx, cy, 10, YELLOW);
  for (int a=0; a<8; a++) {
    float r = a*0.785398f;
    gfx->drawLine(cx+(int16_t)(cosf(r)*14), cy+(int16_t)(sinf(r)*14),
                  cx+(int16_t)(cosf(r)*21), cy+(int16_t)(sinf(r)*21), YELLOW);
    gfx->drawLine(cx+(int16_t)(cosf(r)*14)+1, cy+(int16_t)(sinf(r)*14),
                  cx+(int16_t)(cosf(r)*21)+1, cy+(int16_t)(sinf(r)*21), YELLOW);
  }
}

void drawNoteIcon(int16_t cx, int16_t cy) {
  gfx->fillCircle(cx-5, cy+12, 8, GREEN);
  gfx->fillRect(cx+2,  cy-14, 3, 24, GREEN);
  gfx->fillRect(cx+2,  cy-14, 18, 3,  GREEN);
  gfx->fillCircle(cx+11, cy-14, 4, GREEN);
  gfx->fillRect(cx+8,  cy-14, 3, 12, GREEN);
}

void drawHomePage() {
  gfx->fillScreen(BLACK);

  // Header
  gfx->setFont(FB24); gfx->setTextColor(WHITE); gfx->setTextSize(1);
  gfx->setCursor(20, 58); gfx->print("DIMO");
  gfx->setFont(F9); gfx->setTextColor(DIM);
  gfx->setCursor(122, 58); gfx->print("apps");
  gfx->setFont(FDEF);
  gfx->drawFastHLine(0, 72, SCR_W, 0x2104);

  // Row 1
  drawIconBase(ICO_X[0], ICO_Y1, 0x4A49, "Settings"); drawGearIcon(ICO_X[0], ICO_Y1);
  drawIconBase(ICO_X[1], ICO_Y1, 0x0289, "Clock");    drawClockIcon(ICO_X[1], ICO_Y1);
  drawIconBase(ICO_X[2], ICO_Y1, 0x6200, "Weather");  drawSunIcon(ICO_X[2], ICO_Y1);

  // Row 2
  drawIconBase(ICO_X[0], ICO_Y2, 0x0240, "Music");    drawNoteIcon(ICO_X[0], ICO_Y2);

  // Placeholder spots
  gfx->fillCircle(ICO_X[1], ICO_Y2, ICO_R, 0x1082);
  gfx->setFont(FB12); gfx->setTextColor(DIM);
  gfx->setCursor(ICO_X[1]-8, ICO_Y2+7); gfx->print("+");
  gfx->setFont(F9); gfx->setTextColor(DIM);
  int16_t sx1,sy1; uint16_t sw,sh;
  gfx->getTextBounds("soon", ICO_X[1], ICO_Y2+ICO_R+18, &sx1,&sy1,&sw,&sh);
  gfx->setCursor(ICO_X[1]-(int16_t)sw/2, ICO_Y2+ICO_R+18); gfx->print("soon");

  gfx->fillCircle(ICO_X[2], ICO_Y2, ICO_R, 0x1082);
  gfx->setFont(FB12); gfx->setTextColor(DIM);
  gfx->setCursor(ICO_X[2]-8, ICO_Y2+7); gfx->print("+");
  gfx->setFont(F9); gfx->setTextColor(DIM);
  gfx->getTextBounds("soon", ICO_X[2], ICO_Y2+ICO_R+18, &sx1,&sy1,&sw,&sh);
  gfx->setCursor(ICO_X[2]-(int16_t)sw/2, ICO_Y2+ICO_R+18); gfx->print("soon");
  gfx->setFont(FDEF);

  // Nav hint
  centered("swipe up to return", SCR_H-10, DIM);
}

void handleHomeTap(int16_t tx, int16_t ty) {
  struct { int16_t cx, cy; Page p; } icons[] = {
    {ICO_X[0], ICO_Y1, PAGE_SETTINGS},
    {ICO_X[1], ICO_Y1, PAGE_CLOCK},
    {ICO_X[2], ICO_Y1, PAGE_WEATHER},
    {ICO_X[0], ICO_Y2, PAGE_MUSIC},
  };
  for (int i=0; i<4; i++) {
    int16_t dx=tx-icons[i].cx, dy=ty-icons[i].cy;
    if ((int32_t)dx*dx+(int32_t)dy*dy < (ICO_R+8)*(ICO_R+8)) {
      showPage(icons[i].p);
      return;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Page switcher
// ─────────────────────────────────────────────────────────────────────────────
void showPage(Page p) {
  page = p;
  switch(p){
    case PAGE_FACE:     drawFacePage(); blinkState=BLINK_OPEN; blinkTimer=millis(); nextBlink=5000; break;
    case PAGE_HOME:     drawHomePage(); break;
    case PAGE_CLOCK:    drawClockPage(); break;
    case PAGE_WEATHER:  drawWeatherPage(); break;
    case PAGE_MUSIC:    drawMusicPage(); break;
    case PAGE_SETTINGS: drawSettingsPage(); break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BLE HID
// ─────────────────────────────────────────────────────────────────────────────
class DimoBLE : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConn=true;  if(page==PAGE_MUSIC)drawMusicPage();}
  void onDisconnect(BLEServer*) override { bleConn=false; BLEDevice::startAdvertising(); if(page==PAGE_MUSIC)drawMusicPage();}
};
void sendKey(uint8_t k){
  if(!bleIn||!bleConn)return;
  bleIn->setValue(&k,1);bleIn->notify();
  uint8_t r=0;bleIn->setValue(&r,1);bleIn->notify();
}
void setupBLE(){
  BLEDevice::init("DIMO Remote");
  BLEServer *srv=BLEDevice::createServer();
  srv->setCallbacks(new DimoBLE());
  bleHID=new BLEHIDDevice(srv); bleIn=bleHID->inputReport(1);
  bleHID->manufacturer()->setValue("DIMO");
  bleHID->pnp(0x02,0x045E,0x0000,0x0110);
  bleHID->hidInfo(0x00,0x01);
  const uint8_t rm[]={0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x01,
                      0x15,0x00,0x26,0xFF,0x00,0x75,0x08,0x95,
                      0x01,0x09,0x00,0x81,0x00,0xC0};
  bleHID->reportMap((uint8_t*)rm,sizeof(rm));
  bleHID->startServices();
  BLEAdvertising *adv=BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);
  adv->addServiceUUID(bleHID->hidService()->getUUID());
  adv->start();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch + Swipe
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch(){
  int16_t tx,ty;
  bool pressed=touchRead(tx,ty);
  if(pressed){
    if(!tdDown){
      tdDown=true; tdSX=tx; tdSY=ty; tdLX=tx; tdLY=ty;
      tdMs=millis(); tdSwiped=false;
    } else { tdLX=tx; tdLY=ty; }

    if(!tdSwiped){
      int16_t dx=tdLX-tdSX, dy=tdLY-tdSY;
      bool hSwipe = abs(dx)>45 && abs(dx)>abs(dy);
      bool vSwipe = abs(dy)>50 && abs(dy)>abs(dx);

      if(vSwipe){
        tdSwiped=true;
        if(dy>0 && page==PAGE_FACE){
          // Swipe DOWN on face → open app grid
          showPage(PAGE_HOME);
        } else if(dy<0){
          // Swipe UP → go back
          if(page==PAGE_HOME) showPage(PAGE_FACE);
          else if(page!=PAGE_FACE) showPage(PAGE_HOME);
        }
      } else if(hSwipe){
        // Horizontal — only navigate between content pages opened from home
        tdSwiped=true;
        if(page==PAGE_CLOCK||page==PAGE_WEATHER||page==PAGE_MUSIC){
          const Page ring[]={PAGE_CLOCK,PAGE_WEATHER,PAGE_MUSIC};
          int cur=0; for(int i=0;i<3;i++) if(ring[i]==page){cur=i;break;}
          showPage(dx<0 ? ring[(cur+1)%3] : ring[(cur+2)%3]);
        }
      }
    }

    // Music controls tap (not swipe, within 350ms)
    if(page==PAGE_MUSIC&&!tdSwiped&&millis()-tdMs<350){
      if(tx<120)               sendKey(0xB6);
      else if(tx>SCR_W-120)    sendKey(0xB5);
      else if(abs(tx-SCR_W/2)<60){blePlaying=!blePlaying;sendKey(0xCD);drawMusicPage();}
    }
  } else {
    if(tdDown&&!tdSwiped){
      if(page==PAGE_HOME)          handleHomeTap(tdLX,tdLY);
      else if(page==PAGE_SETTINGS) handleSettingsTap(tdLY);
    }
    tdDown=false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup(){
  USBSerial.begin(115200);
  delay(200);

  Wire.begin(SDA_PIN,SCL_PIN);
  expander.begin(0x20);
  expander.pinMode(4,OUTPUT); expander.pinMode(5,OUTPUT);
  expander.digitalWrite(4,HIGH); expander.digitalWrite(5,HIGH);
  delay(10);

  // Load saved WiFi credentials from NVS
  prefs.begin("dimo", true);
  String s=prefs.getString("ssid",""); String p=prefs.getString("pass","");
  prefs.end();
  if(s.length()>0){ s.toCharArray(wifiSSID,sizeof(wifiSSID)); p.toCharArray(wifiPass,sizeof(wifiPass)); }

  gfx->begin();
  gfx->setBrightness(230);
  gfx->fillScreen(BLACK);

  bootAnimation();

  WiFi.onEvent([](WiFiEvent_t ev,WiFiEventInfo_t info){
    if(ev==ARDUINO_EVENT_WIFI_STA_GOT_IP){wifiOk=true;wifiConnecting=false;configTzTime(TZ_INFO,"pool.ntp.org","time.nist.gov");}
    else if(ev==ARDUINO_EVENT_WIFI_STA_DISCONNECTED){wifiOk=false;wifiConnecting=false;}
  });
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(wifiSSID,wifiPass);

  wifiAnimation();
  setupBLE();

  showPage(PAGE_FACE);
  USBSerial.println("DIMO ready");
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop(){
  uint32_t now=millis();

  // WiFi portal — handle requests + cancel tap
  if(portalActive){
    portalServer.handleClient();
    int16_t tx,ty;
    if(touchRead(tx,ty) && ty>360){ // tap cancel button area
      delay(300);
      stopPortal();
      showPage(PAGE_SETTINGS);
    }
    delay(10);
    return; // skip all other logic while portal is running
  }

  handleTouch();

  // ── FACE: talking frame demo from demo.zip ────────────────────────────────
  if(page==PAGE_FACE){
    if (now - talkFrameTimer >= TALK_FRAME_MS) {
      talkFrameTimer = now;
      talkFrameIndex = (talkFrameIndex + 1) % TALK_FRAME_COUNT;
      drawTalkFrame(talkFrameIndex);
    }
  }

  // ── Clock ─────────────────────────────────────────────────────────────────
  static uint32_t lastClk=0;
  if(page==PAGE_CLOCK&&now-lastClk>1000){lastClk=now;drawClockPage();}

  // ── WiFi reconnect ────────────────────────────────────────────────────────
  static uint32_t lastWifiTry=0;
  if(!wifiOk&&now-lastWifiTry>12000){lastWifiTry=now;WiFi.begin(wifiSSID,wifiPass);}

  // ── Weather ───────────────────────────────────────────────────────────────
  if(wifiOk&&!wxFetched){
    wxFetched=true; lastWx=now;
    HTTPClient http; http.setTimeout(5000);
    if(http.begin(OWM_URL)&&http.GET()==200){
      String b=http.getString();
      float tmp=jsonF(b,"temp"); String dsc=jsonS(b,"description");
      if(!isnan(tmp)&&dsc.length()>0){wxTemp=String((int)round(tmp));wxDesc=dsc;wxOk=true;}
    }
    http.end();
    if(page==PAGE_WEATHER)drawWeatherPage(); else drawStatusBar();
  }
  if(wifiOk&&now-lastWx>600000){lastWx=now;wxFetched=false;}

  delay(16);
}

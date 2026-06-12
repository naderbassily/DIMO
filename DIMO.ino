/*
 * DIMO — Robot companion, Waveshare ESP32-C6 Touch AMOLED 1.8"
 * 368x448 SH8601 QSPI | FT3168 touch | XCA9554 power
 *
 * Flicker-free: static body drawn once, only eye/mouth regions redrawn on change.
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
#define W        368
#define H        448

Adafruit_XCA9554 expander;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(5,0,1,2,3,4);
Arduino_SH8601  *gfx = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, W, H);

// ── Credentials ───────────────────────────────────────────────────────────────
const char *WIFI_SSID = "Bassily - IoT";
const char *WIFI_PASS = "@Bassily199711412";
const char *OWM_URL   = "http://api.openweathermap.org/data/2.5/weather"
                        "?q=Cumming,GA,US&units=imperial"
                        "&appid=898dec73df9c4e262a862baa0c11028f";
const char *TZ_INFO   = "EST5EDT,M3.2.0/2,M11.1.0/2";

// ── Colors ────────────────────────────────────────────────────────────────────
#define BG      0x0821   // very dark blue-black
#define WHITE   0xFFFF
#define GRAY    0x7BEF
#define DIM     0x39E7
#define YELLOW  0xFFE0
#define CYAN    0x07FF
#define PINK    0xFB56
#define RED     0xF800
#define GREEN   0x07E0
#define ORANGE  0xFD20
#define PURPLE  0x801F
#define LBLUE   0x3DFF  // light blue body
#define DBODY   0x1082  // dark body
#define CHEEK   0xFB8C

// ── Pages & moods ─────────────────────────────────────────────────────────────
enum Page { PAGE_FACE, PAGE_CLOCK, PAGE_WEATHER, PAGE_MUSIC };
Page page = PAGE_FACE;

enum Mood { MOOD_HAPPY, MOOD_EXCITED, MOOD_CUTE, MOOD_THINKING,
            MOOD_SLEEPY, MOOD_SAD, MOOD_SURPRISED, MOOD_ANGRY,
            MOOD_LOVE, MOOD_PARTY };
Mood mood     = MOOD_HAPPY;
Mood prevMood = (Mood)-1;

// ── Face dirty flags — only redraw what changed ───────────────────────────────
bool dirtyEyes   = true;
bool dirtyMouth  = true;
bool dirtyBody   = true;   // first draw
bool dirtyStatus = true;

// ── Blink state ───────────────────────────────────────────────────────────────
bool     blinking    = false;
bool     prevBlink   = false;
uint32_t lastBlink   = 0;
uint32_t blinkStart  = 0;

// ── Eye wander ────────────────────────────────────────────────────────────────
int8_t   eyeOX = 0, eyeOY = 0;
int8_t   prevEyeOX = 99;
uint32_t lastEyeMove = 0;

// ── Mood auto-cycle ───────────────────────────────────────────────────────────
uint32_t lastMoodChange = 0;
uint32_t moodDuration   = 9000;

// ── WiFi / Weather ────────────────────────────────────────────────────────────
bool     wifiOk     = false;
String   wxTemp     = "--";
String   wxDesc     = "loading";
bool     wxOk       = false;
uint32_t lastWxFetch= 0;
bool     wxFetched  = false;

// ── Touch ─────────────────────────────────────────────────────────────────────
bool     tdDown      = false;
int16_t  tdStartX    = 0, tdStartY = 0;
int16_t  tdLastX     = 0, tdLastY  = 0;
uint32_t tdStartMs   = 0;
bool     tdSwiped    = false;

// ── BLE ───────────────────────────────────────────────────────────────────────
bool              bleConn  = false;
BLEHIDDevice     *bleHID   = nullptr;
BLECharacteristic*bleIn    = nullptr;
bool              blePlaying= false;

// ─────────────────────────────────────────────────────────────────────────────
//  Geometry constants (portrait 368×448)
// ─────────────────────────────────────────────────────────────────────────────
// Head box: x=44 y=60 w=280 h=240
#define HEAD_X   44
#define HEAD_Y   60
#define HEAD_W   280
#define HEAD_H   240
#define HEAD_R   40

// Eyes (centres)
#define EL_X     130
#define ER_X     238
#define E_Y      172    // eye centre Y
#define EYE_RX   32     // eye X radius (normal)
#define EYE_RY   30     // eye Y radius (normal)

// Mouth
#define M_X      184
#define M_Y      252

// Cheeks
#define CL_X     100
#define CR_X     268
#define C_Y      210

// Antenna
#define ANT_X    184
#define ANT_TOP  18
#define ANT_H    44

// Torso
#define TOR_X    104
#define TOR_Y    310
#define TOR_W    160
#define TOR_H    72

// Status bar
#define SBAR_H   40

// ─────────────────────────────────────────────────────────────────────────────
//  Tiny helpers
// ─────────────────────────────────────────────────────────────────────────────
void centered(const char *s, int16_t y, uint16_t col, uint8_t sz=1) {
  gfx->setTextSize(sz);
  gfx->setTextColor(col);
  gfx->setCursor((W - strlen(s)*6*sz)/2, y);
  gfx->print(s);
}

float jsonF(const String &b, const char *k) {
  String key = String("\"")+k+"\":";
  int i = b.indexOf(key);
  if (i<0) return NAN;
  return b.substring(i+key.length(), i+key.length()+10).toFloat();
}
String jsonS(const String &b, const char *k) {
  String key = String("\"")+k+"\":\"";
  int i = b.indexOf(key);
  if (i<0) return "";
  int s=i+key.length(), e=b.indexOf('"',s);
  return b.substring(s,e);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch read (FT3168 I2C 0x38)
// ─────────────────────────────────────────────────────────────────────────────
bool touchRead(int16_t &x, int16_t &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false)!=0) return false;
  Wire.requestFrom(0x38,6);
  if (Wire.available()<6) return false;
  uint8_t td=Wire.read(), xh=Wire.read(), xl=Wire.read(),
             yh=Wire.read(), yl=Wire.read();
  Wire.read();
  if ((td&0x0F)==0) return false;
  if ((xh&0xC0)==0x80) return false;
  x=((xh&0x0F)<<8)|xl;
  y=((yh&0x0F)<<8)|yl;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw static body (head shell + torso + antenna) — called once per page switch
// ─────────────────────────────────────────────────────────────────────────────
void drawBody() {
  gfx->fillScreen(BG);

  // Antenna pole
  gfx->fillRect(ANT_X-3, ANT_TOP, 6, ANT_H, GRAY);
  // Antenna bulb
  gfx->fillCircle(ANT_X, ANT_TOP-2, 11, CYAN);
  gfx->drawCircle(ANT_X, ANT_TOP-2, 11, WHITE);
  gfx->fillCircle(ANT_X, ANT_TOP-2, 5, WHITE);

  // Head shell
  gfx->fillRoundRect(HEAD_X, HEAD_Y, HEAD_W, HEAD_H, HEAD_R, DBODY);
  gfx->drawRoundRect(HEAD_X, HEAD_Y, HEAD_W, HEAD_H, HEAD_R, GRAY);
  gfx->drawRoundRect(HEAD_X+2, HEAD_Y+2, HEAD_W-4, HEAD_H-4, HEAD_R-2, DIM);

  // Ear nubs
  gfx->fillRoundRect(HEAD_X-14, E_Y-14, 16, 28, 6, DBODY);
  gfx->drawRoundRect(HEAD_X-14, E_Y-14, 16, 28, 6, GRAY);
  gfx->fillRoundRect(HEAD_X+HEAD_W-2, E_Y-14, 16, 28, 6, DBODY);
  gfx->drawRoundRect(HEAD_X+HEAD_W-2, E_Y-14, 16, 28, 6, GRAY);

  // Torso
  gfx->fillRoundRect(TOR_X, TOR_Y, TOR_W, TOR_H, 16, DBODY);
  gfx->drawRoundRect(TOR_X, TOR_Y, TOR_W, TOR_H, 16, GRAY);
  // chest light
  gfx->fillCircle(W/2, TOR_Y+22, 10, CYAN);
  gfx->drawCircle(W/2, TOR_Y+22, 10, WHITE);
  gfx->fillCircle(W/2, TOR_Y+22, 5, WHITE);
  // belly buttons
  for (int i=-1;i<=1;i++)
    gfx->fillRoundRect(W/2+i*28-8, TOR_Y+46, 16, 10, 4, DIM);

  // Name tag
  gfx->setTextColor(CYAN); gfx->setTextSize(1);
  gfx->setCursor(W/2-14, TOR_Y+TOR_H+8);
  gfx->print("DIMO");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw eye region (clears old eye area first)
// ─────────────────────────────────────────────────────────────────────────────
void drawEyeAt(int16_t cx, int16_t cy, bool closed, bool squint,
               bool wide, bool heart, bool angry, int8_t ox, int8_t oy) {
  // Clear eye area
  int16_t ew = 38, eh = 40;
  gfx->fillRect(cx-ew, cy-eh-2, ew*2, eh*2+24, DBODY);

  if (closed) {
    // closed line
    gfx->fillRoundRect(cx-28, cy-3, 56, 7, 3, WHITE);
    return;
  }

  int16_t rx = wide ? 36 : (squint ? EYE_RX : EYE_RX);
  int16_t ry = squint ? 10 : (wide ? 36 : EYE_RY);

  if (heart) {
    // Heart eyes
    gfx->fillCircle(cx-9, cy-4, 12, PINK);
    gfx->fillCircle(cx+9, cy-4, 12, PINK);
    gfx->fillTriangle(cx-20, cy+2, cx+20, cy+2, cx, cy+20, PINK);
    gfx->fillCircle(cx, cy+4, 4, PINK);
  } else {
    // Eye white
    gfx->fillEllipse(cx+ox, cy+oy, rx, ry, WHITE);

    if (angry) {
      // squinted angry — fill top half dark
      gfx->fillRect(cx+ox-rx, cy+oy-ry, rx*2, ry, DBODY);
      gfx->fillEllipse(cx+ox, cy+oy, rx, ry/2+2, WHITE);
    }

    // Pupil
    gfx->fillCircle(cx+ox+3, cy+oy+3, squint ? 5 : 11, 0x1082);
    // Shine
    gfx->fillCircle(cx+ox+7, cy+oy-2, squint ? 2 : 4, WHITE);
  }

  // Eyelid top line
  if (!squint && !wide)
    gfx->drawFastHLine(cx+ox-rx, cy+oy-ry, rx*2, DIM);
}

void drawEyes() {
  bool cl = blinking;
  switch (mood) {
    case MOOD_HAPPY:
      drawEyeAt(EL_X, E_Y, cl, false, false, false, false, eyeOX, eyeOY);
      drawEyeAt(ER_X, E_Y, cl, false, false, false, false, eyeOX, eyeOY);
      break;
    case MOOD_EXCITED:
      drawEyeAt(EL_X, E_Y, cl, false, true, false, false, eyeOX, eyeOY);
      drawEyeAt(ER_X, E_Y, cl, false, true, false, false, eyeOX, eyeOY);
      break;
    case MOOD_CUTE:
      drawEyeAt(EL_X, E_Y, cl, true, false, false, false, 0, 0);
      drawEyeAt(ER_X, E_Y, cl, true, false, false, false, 0, 0);
      break;
    case MOOD_THINKING:
      drawEyeAt(EL_X, E_Y, cl, true, false, false, false, 4, 0);
      drawEyeAt(ER_X, E_Y, cl, false, false, false, false, 4, 0);
      break;
    case MOOD_SLEEPY:
      drawEyeAt(EL_X, E_Y, true, false, false, false, false, 0, 0);
      drawEyeAt(ER_X, E_Y, true, false, false, false, false, 0, 0);
      // Zzz
      gfx->setTextColor(DIM); gfx->setTextSize(2);
      gfx->setCursor(ER_X+22, E_Y-38); gfx->print("z");
      gfx->setTextSize(3);
      gfx->setCursor(ER_X+36, E_Y-62); gfx->print("Z");
      break;
    case MOOD_SAD:
      drawEyeAt(EL_X, E_Y, cl, true, false, false, false, 0, 0);
      drawEyeAt(ER_X, E_Y, cl, true, false, false, false, 0, 0);
      break;
    case MOOD_SURPRISED:
      drawEyeAt(EL_X, E_Y, false, false, true, false, false, 0, 0);
      drawEyeAt(ER_X, E_Y, false, false, true, false, false, 0, 0);
      // eyebrows up
      gfx->fillRoundRect(EL_X-26, E_Y-52, 52, 7, 3, WHITE);
      gfx->fillRoundRect(ER_X-26, E_Y-52, 52, 7, 3, WHITE);
      break;
    case MOOD_ANGRY:
      drawEyeAt(EL_X, E_Y, cl, false, false, false, true, 0, 0);
      drawEyeAt(ER_X, E_Y, cl, false, false, false, true, 0, 0);
      // angry brows
      gfx->fillRect(EL_X-24, E_Y-46, 48, 6, RED);
      for (int i=0;i<6;i++) gfx->drawPixel(EL_X-24+i*2, E_Y-46-i/2, RED);
      gfx->fillRect(ER_X-24, E_Y-46, 48, 6, RED);
      for (int i=0;i<6;i++) gfx->drawPixel(ER_X+24-i*2, E_Y-46-i/2, RED);
      break;
    case MOOD_LOVE:
      drawEyeAt(EL_X, E_Y, false, false, false, true, false, 0, 0);
      drawEyeAt(ER_X, E_Y, false, false, false, true, false, 0, 0);
      break;
    case MOOD_PARTY:
      drawEyeAt(EL_X, E_Y, cl, false, true, false, false, eyeOX, eyeOY);
      drawEyeAt(ER_X, E_Y, cl, false, true, false, false, eyeOX, eyeOY);
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw mouth + cheeks (clears region first)
// ─────────────────────────────────────────────────────────────────────────────
void drawMouth() {
  // Clear mouth + cheek zone
  gfx->fillRect(HEAD_X+4, M_Y-30, HEAD_W-8, 80, DBODY);

  switch (mood) {
    case MOOD_HAPPY:
    case MOOD_EXCITED:
    case MOOD_PARTY: {
      // big smile arc
      int r = (mood==MOOD_HAPPY) ? 30 : 40;
      for (int x=-r; x<=r; x++) {
        int dy = (int)((float)x*x*18/r/r);
        gfx->fillRect(M_X+x, M_Y+dy, 1, 3, WHITE);
      }
      if (mood==MOOD_EXCITED || mood==MOOD_PARTY) {
        // cheeks
        gfx->fillCircle(CL_X, C_Y, 18, CHEEK);
        gfx->fillCircle(CR_X, C_Y, 18, CHEEK);
        gfx->fillCircle(CL_X, C_Y, 10, 0xFDB0);
        gfx->fillCircle(CR_X, C_Y, 10, 0xFDB0);
      }
      break;
    }
    case MOOD_CUTE: {
      // small w-mouth
      gfx->fillRoundRect(M_X-20, M_Y-4, 40, 12, 5, PINK);
      gfx->fillCircle(CL_X, C_Y, 16, CHEEK);
      gfx->fillCircle(CR_X, C_Y, 16, CHEEK);
      break;
    }
    case MOOD_THINKING: {
      // flat line offset
      gfx->fillRoundRect(M_X-22, M_Y, 44, 7, 3, GRAY);
      // thought bubble
      gfx->fillCircle(ER_X+26, E_Y-18, 5, DIM);
      gfx->fillCircle(ER_X+40, E_Y-34, 7, DIM);
      gfx->fillCircle(ER_X+54, E_Y-54, 11, DIM);
      break;
    }
    case MOOD_SLEEPY: {
      gfx->fillRoundRect(M_X-18, M_Y+8, 36, 7, 3, DIM);
      break;
    }
    case MOOD_SAD: {
      // downward arc
      int r=30;
      for (int x=-r;x<=r;x++) {
        int dy = -(int)((float)x*x*14/r/r)+16;
        gfx->fillRect(M_X+x, M_Y+dy, 1, 3, GRAY);
      }
      // tears
      gfx->fillRect(EL_X+6, E_Y+EYE_RY, 4, 20, CYAN);
      gfx->fillCircle(EL_X+8, E_Y+EYE_RY+20, 5, CYAN);
      gfx->fillRect(ER_X+6, E_Y+EYE_RY, 4, 20, CYAN);
      gfx->fillCircle(ER_X+8, E_Y+EYE_RY+20, 5, CYAN);
      break;
    }
    case MOOD_SURPRISED: {
      gfx->fillEllipse(M_X, M_Y+4, 20, 28, WHITE);
      gfx->fillEllipse(M_X, M_Y-10, 20, 8, DBODY);
      break;
    }
    case MOOD_ANGRY: {
      gfx->fillRoundRect(M_X-26, M_Y, 52, 8, 3, RED);
      // steam
      for (int s=-1;s<=1;s++) {
        gfx->fillRect(M_X+s*20-2, M_Y-22, 4, 18, RED);
      }
      break;
    }
    case MOOD_LOVE: {
      int r=30;
      for (int x=-r;x<=r;x++) {
        int dy=(int)((float)x*x*18/r/r);
        gfx->fillRect(M_X+x, M_Y+dy, 1, 3, PINK);
      }
      gfx->fillCircle(CL_X, C_Y, 16, 0xF810);
      gfx->fillCircle(CR_X, C_Y, 16, 0xF810);
      // floating hearts
      gfx->setTextColor(PINK); gfx->setTextSize(2);
      gfx->setCursor(EL_X-44, E_Y-68); gfx->print("<3");
      gfx->setCursor(ER_X+18, E_Y-78); gfx->print("<3");
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Status bar
// ─────────────────────────────────────────────────────────────────────────────
void drawStatusBar() {
  gfx->fillRect(0, 0, W, SBAR_H, 0x0000);

  struct tm t;
  if (getLocalTime(&t, 30)) {
    char buf[8];
    snprintf(buf,sizeof(buf),"%02d:%02d",t.tm_hour,t.tm_min);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(6, 12); gfx->print(buf);
  }

  // WiFi indicator
  uint16_t wc = wifiOk ? GREEN : RED;
  gfx->fillCircle(W/2, 20, 6, wc);

  if (wxOk) {
    String s = wxTemp + "F";
    gfx->setTextColor(CYAN); gfx->setTextSize(2);
    gfx->setCursor(W - (int16_t)s.length()*12 - 8, 12);
    gfx->print(s);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Face page — smart partial redraw
// ─────────────────────────────────────────────────────────────────────────────
void drawFacePage() {
  if (dirtyBody) {
    dirtyBody  = false;
    dirtyEyes  = true;
    dirtyMouth = true;
    drawBody();
    drawStatusBar();
  }
  if (dirtyEyes) {
    dirtyEyes = false;
    drawEyes();
  }
  if (dirtyMouth) {
    dirtyMouth = false;
    drawMouth();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Clock page
// ─────────────────────────────────────────────────────────────────────────────
void drawClockPage() {
  gfx->fillScreen(BG);

  struct tm t;
  if (!getLocalTime(&t, 200)) {
    centered("Syncing time...", 200, DIM, 2);
    drawStatusBar();
    return;
  }

  char tb[8], db[24];
  snprintf(tb,sizeof(tb),"%02d:%02d",t.tm_hour,t.tm_min);
  strftime(db,sizeof(db),"%A  %b %d",&t);

  // Big time
  gfx->setTextColor(WHITE); gfx->setTextSize(5);
  int16_t tw = strlen(tb)*30;
  gfx->setCursor((W-tw)/2, 140); gfx->print(tb);

  // Date
  centered(db, 220, GRAY, 2);

  // Seconds ring
  int sec = t.tm_sec;
  gfx->drawCircle(W/2, 320, 70, DIM);
  float ang = (sec/60.0f)*2*PI - PI/2;
  int rx = W/2 + (int)(70*cos(ang));
  int ry = 320  + (int)(70*sin(ang));
  gfx->fillCircle(W/2, 320, 4, DIM);
  gfx->fillCircle(rx, ry, 8, CYAN);
  gfx->drawCircle(rx, ry, 8, WHITE);

  // Second number
  char sb[4]; snprintf(sb,sizeof(sb),"%02d",sec);
  centered(sb, 308, GRAY, 2);

  drawStatusBar();
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< face");
  gfx->setCursor(W-60,H-14); gfx->print("weather >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Weather page
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  gfx->fillScreen(BG);
  drawStatusBar();

  centered("Cumming, GA", 52, GRAY, 1);

  if (!wifiOk) { centered("No WiFi", 200, RED, 2); goto hints; }
  if (!wxOk)   { centered("Loading...", 200, DIM, 2); goto hints; }

  {
    String ts = wxTemp + " F";
    gfx->setTextColor(YELLOW); gfx->setTextSize(5);
    gfx->setCursor((W - ts.length()*30)/2, 110); gfx->print(ts);

    String dc = wxDesc; dc[0]=toupper(dc[0]);
    centered(dc.c_str(), 200, CYAN, 2);

    // Weather icon
    int16_t ix=W/2, iy=300;
    if (wxDesc.indexOf("clear")>=0||wxDesc.indexOf("sun")>=0) {
      gfx->fillCircle(ix,iy,36,YELLOW);
      for (int a=0;a<360;a+=45) {
        gfx->drawLine(ix+(int)(44*cos(a*PI/180)),iy+(int)(44*sin(a*PI/180)),
                      ix+(int)(58*cos(a*PI/180)),iy+(int)(58*sin(a*PI/180)),YELLOW);
        gfx->drawLine(ix+(int)(45*cos(a*PI/180)),iy+(int)(45*sin(a*PI/180)),
                      ix+(int)(57*cos(a*PI/180)),iy+(int)(57*sin(a*PI/180)),YELLOW);
      }
    } else if (wxDesc.indexOf("rain")>=0||wxDesc.indexOf("drizzle")>=0||
               wxDesc.indexOf("storm")>=0) {
      gfx->fillEllipse(ix,iy-8,44,26,GRAY);
      gfx->fillEllipse(ix-16,iy-18,28,18,GRAY);
      for (int d=-2;d<=2;d++) {
        gfx->fillRect(ix+d*16-2,iy+24,4,20,CYAN);
      }
    } else {
      gfx->fillEllipse(ix,iy,44,24,GRAY);
      gfx->fillEllipse(ix-16,iy-12,28,18,GRAY);
      gfx->fillEllipse(ix+14,iy-14,22,16,GRAY);
    }
  }

  hints:
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< clock");
  gfx->setCursor(W-52,H-14); gfx->print("face >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Music page
// ─────────────────────────────────────────────────────────────────────────────
void drawMusicPage() {
  gfx->fillScreen(BG);
  drawStatusBar();
  centered("DIMO Music", 54, PURPLE, 2);

  // Album art
  gfx->fillRoundRect(84,90,200,160,18,0x1810);
  gfx->drawRoundRect(84,90,200,160,18,PURPLE);
  gfx->fillCircle(W/2,170,44,PURPLE);
  gfx->fillCircle(W/2,170,44,0x300C);
  gfx->fillCircle(W/2,170,14,BG);
  gfx->drawCircle(W/2,170,44,0x600C);

  if (bleConn) {
    centered("Connected", 268, GREEN, 1);
  } else {
    centered("Pair: DIMO Remote", 265, DIM, 1);
  }

  // Controls row
  int16_t cy=340;
  // Prev
  gfx->fillTriangle(52,cy-22,52,cy+22,30,cy,WHITE);
  gfx->fillRect(28,cy-22,8,44,WHITE);
  // Play/Pause
  if (blePlaying) {
    gfx->fillRoundRect(W/2-22,cy-24,16,48,3,GREEN);
    gfx->fillRoundRect(W/2+6, cy-24,16,48,3,GREEN);
  } else {
    gfx->fillTriangle(W/2-18,cy-24,W/2-18,cy+24,W/2+22,cy,GREEN);
  }
  // Next
  gfx->fillTriangle(316,cy-22,316,cy+22,338,cy,WHITE);
  gfx->fillRect(332,cy-22,8,44,WHITE);

  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< face");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Page switcher
// ─────────────────────────────────────────────────────────────────────────────
void showPage(Page p) {
  page = p;
  if (p==PAGE_FACE) { dirtyBody=true; dirtyEyes=true; dirtyMouth=true; }
  switch(p) {
    case PAGE_FACE:    drawFacePage(); break;
    case PAGE_CLOCK:   drawClockPage(); break;
    case PAGE_WEATHER: drawWeatherPage(); break;
    case PAGE_MUSIC:   drawMusicPage(); break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BLE HID setup
// ─────────────────────────────────────────────────────────────────────────────
#define KEY_PLAY_PAUSE  0xCD
#define KEY_NEXT        0xB5
#define KEY_PREV        0xB6

class DimoServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConn=true; if(page==PAGE_MUSIC) drawMusicPage(); }
  void onDisconnect(BLEServer*) override {
    bleConn=false;
    BLEDevice::startAdvertising();
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
  srv->setCallbacks(new DimoServerCB());
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
  BLEAdvertising *adv=BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);
  adv->addServiceUUID(bleHID->hidService()->getUUID());
  adv->start();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch handler
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch() {
  int16_t tx,ty;
  bool pressed = touchRead(tx,ty);

  if (pressed) {
    if (!tdDown) {
      tdDown=true; tdStartX=tx; tdStartY=ty;
      tdLastX=tx;  tdLastY=ty;
      tdStartMs=millis(); tdSwiped=false;
    } else {
      tdLastX=tx; tdLastY=ty;
    }
    if (!tdSwiped) {
      int16_t dx=tdLastX-tdStartX, dy=tdLastY-tdStartY;
      if (abs(dx)>55 && abs(dx)>abs(dy)*1.4f) {
        tdSwiped=true;
        if (dx<0) showPage((Page)((page+1)%4));
        else      showPage((Page)((page+3)%4));
      }
    }
    // Music taps
    if (page==PAGE_MUSIC && !tdSwiped && millis()-tdStartMs<250) {
      if      (tx<110)      sendKey(KEY_PREV);
      else if (tx>W-110)    sendKey(KEY_NEXT);
      else if (abs(tx-W/2)<50) {
        blePlaying=!blePlaying;
        sendKey(KEY_PLAY_PAUSE);
        drawMusicPage();
      }
    }
  } else {
    tdDown=false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  USBSerial.begin(115200);
  delay(300);

  Wire.begin(SDA_PIN, SCL_PIN);
  expander.begin(0x20);
  expander.pinMode(4,OUTPUT); expander.pinMode(5,OUTPUT);
  expander.digitalWrite(4,HIGH); expander.digitalWrite(5,HIGH);
  delay(10);

  gfx->begin();
  gfx->setBrightness(230);
  gfx->fillScreen(BG);

  // Boot splash
  gfx->setTextColor(CYAN); gfx->setTextSize(6);
  gfx->setCursor((W-6*36)/2, 170); gfx->print("DIMO");
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  centered("robot companion", 248, DIM, 1);
  delay(1400);

  // WiFi
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
  WiFi.begin(WIFI_SSID,WIFI_PASS);

  setupBLE();

  dirtyBody=true;
  showPage(PAGE_FACE);
  USBSerial.println("DIMO ready");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  handleTouch();

  // ── Face page animations (partial redraws only) ──────────────────────────
  if (page==PAGE_FACE) {

    // Blink logic
    if (!blinking && now-lastBlink > (uint32_t)random(3500,7000)) {
      blinking=true; blinkStart=now;
    }
    if (blinking && now-blinkStart>130) {
      blinking=false; lastBlink=now;
    }
    if (blinking!=prevBlink) {
      prevBlink=blinking;
      dirtyEyes=true;
    }

    // Eye wander
    if (now-lastEyeMove>(uint32_t)random(2500,6000)) {
      lastEyeMove=now;
      eyeOX=random(-7,8); eyeOY=random(-4,5);
      if (eyeOX!=prevEyeOX) { prevEyeOX=eyeOX; dirtyEyes=true; }
    }

    // Auto mood
    if (now-lastMoodChange>moodDuration) {
      lastMoodChange=now;
      moodDuration=random(7000,15000);
      Mood m[]={MOOD_HAPPY,MOOD_EXCITED,MOOD_CUTE,MOOD_THINKING,
                MOOD_SLEEPY,MOOD_SAD,MOOD_SURPRISED,MOOD_LOVE,MOOD_PARTY};
      Mood next=m[random(0,9)];
      if (next!=mood) {
        mood=next;
        dirtyEyes=true;
        dirtyMouth=true;
      }
    }

    drawFacePage();

    // Status bar once per second
    static uint32_t lastStatus=0;
    if (now-lastStatus>1000) { lastStatus=now; dirtyStatus=true; }
    if (dirtyStatus) { dirtyStatus=false; drawStatusBar(); }
  }

  // ── Clock page: refresh every second ─────────────────────────────────────
  static uint32_t lastClkDraw=0;
  if (page==PAGE_CLOCK && now-lastClkDraw>1000) {
    lastClkDraw=now; drawClockPage();
  }

  // ── WiFi reconnect ────────────────────────────────────────────────────────
  static uint32_t lastWifiTry=0;
  if (!wifiOk && now-lastWifiTry>12000) {
    lastWifiTry=now;
    WiFi.begin(WIFI_SSID,WIFI_PASS);
  }

  // ── Weather fetch ─────────────────────────────────────────────────────────
  if (wifiOk && !wxFetched) {
    wxFetched=true; lastWxFetch=now;
    HTTPClient http;
    http.setTimeout(5000);
    if (http.begin(OWM_URL)) {
      if (http.GET()==200) {
        String b=http.getString();
        float t=jsonF(b,"temp");
        String d=jsonS(b,"description");
        if (!isnan(t)&&d.length()>0) {
          wxTemp=String((int)round(t)); wxDesc=d; wxOk=true;
        }
      }
      http.end();
    }
    if (page==PAGE_WEATHER) drawWeatherPage();
    else drawStatusBar();
  }
  if (wifiOk && now-lastWxFetch>600000) {
    lastWxFetch=now; wxFetched=false;
  }

  delay(20);
}

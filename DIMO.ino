/*
 * DIMO — Desktop Robot Companion
 * Waveshare ESP32-C6 Touch AMOLED 1.8" (368x448)
 * Phase 1: Face + Clock + Weather + Touch navigation
 *
 * Face fills the screen — no body drawn, just expressive face on black.
 * Partial redraws only — no flicker.
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

// ── Color palette (matches reference) ─────────────────────────────────────────
#define BLACK     0x0000
#define WHITE     0xFFFF
#define ECYAN     0x07FF   // eye cyan
#define ECYAN2    0x05DF   // eye mid glow
#define ECYAN3    0x0410   // eye outer glow
#define ECYAN4    0x020C   // eye halo
#define EWHITE    0xCFFF   // eye inner bright
#define PINK      0xFB4C   // cheeks / love
#define PINK2     0xF8A0   // cheek inner
#define RED       0xF800
#define GREEN     0x07E0
#define YELLOW    0xFFE0
#define GRAY      0x7BEF
#define DIM       0x2965
#define CYAN      0x07FF
#define PURPLE    0xC81F
#define ORANGE    0xFD20
#define DBLUE     0x000D   // very dark blue (status bar bg)

// ── Face layout constants ─────────────────────────────────────────────────────
#define ELX   124    // left eye centre X
#define ERX   244    // right eye centre X
#define EY    188    // eye centre Y
#define CKL   88     // left cheek X
#define CKR   280    // right cheek X
#define CKY   248    // cheek Y
#define MX    184    // mouth centre X
#define MY    318    // mouth centre Y
#define SBAR  32     // status bar height

// ── Pages ─────────────────────────────────────────────────────────────────────
enum Page { PAGE_FACE, PAGE_CLOCK, PAGE_WEATHER, PAGE_MUSIC };
Page page = PAGE_FACE;

// ── Moods ─────────────────────────────────────────────────────────────────────
enum Mood {
  MOOD_HAPPY, MOOD_EXCITED, MOOD_CUTE, MOOD_THINKING,
  MOOD_SLEEPY, MOOD_SAD, MOOD_SURPRISED, MOOD_ANGRY,
  MOOD_LOVE, MOOD_PARTY
};
Mood mood     = MOOD_HAPPY;
Mood prevMood = (Mood)255;

// ── Dirty flags (partial redraws) ─────────────────────────────────────────────
bool dirtyAll    = true;
bool dirtyEyes   = true;
bool dirtyMouth  = true;
bool dirtyStatus = true;

// ── Blink ─────────────────────────────────────────────────────────────────────
bool     blinking   = false;
bool     prevBlink  = false;
uint32_t lastBlink  = 0;
uint32_t blinkStart = 0;

// ── Eye wander ────────────────────────────────────────────────────────────────
int8_t   eyeOX=0, eyeOY=0;
int8_t   pEyeOX=99;
uint32_t lastEyeMove=0;

// ── Mood cycle ────────────────────────────────────────────────────────────────
uint32_t lastMoodChange=0;
uint32_t moodDur=10000;

// ── WiFi / weather ────────────────────────────────────────────────────────────
bool     wifiOk=false;
String   wxTemp="--";
String   wxDesc="";
bool     wxOk=false;
bool     wxFetched=false;
uint32_t lastWxFetch=0;

// ── Touch ─────────────────────────────────────────────────────────────────────
bool     tdDown=false;
int16_t  tdSX=0, tdSY=0, tdLX=0, tdLY=0;
uint32_t tdMs=0;
bool     tdSwiped=false;

// ── BLE ───────────────────────────────────────────────────────────────────────
bool              bleConn=false;
BLEHIDDevice     *bleHID=nullptr;
BLECharacteristic*bleIn=nullptr;
bool              blePlaying=false;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
void centered(const char *s, int16_t y, uint16_t col, uint8_t sz=1) {
  gfx->setTextSize(sz);
  gfx->setTextColor(col);
  gfx->setCursor((W-(int16_t)(strlen(s)*6*sz))/2, y);
  gfx->print(s);
}

float jsonF(const String &b, const char *k) {
  String key=String("\"")+k+"\":";
  int i=b.indexOf(key); if(i<0) return NAN;
  return b.substring(i+key.length(),i+key.length()+10).toFloat();
}
String jsonS(const String &b, const char *k) {
  String key=String("\"")+k+"\":\"";
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
  if (Wire.endTransmission(false)!=0) return false;
  Wire.requestFrom(0x38,6);
  if (Wire.available()<6) return false;
  uint8_t td=Wire.read(),xh=Wire.read(),xl=Wire.read(),
             yh=Wire.read(),yl=Wire.read();
  Wire.read();
  if((td&0x0F)==0) return false;
  if((xh&0xC0)==0x80) return false;
  x=((xh&0x0F)<<8)|xl; y=((yh&0x0F)<<8)|yl;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Eye drawing — glowing cyan orbs matching reference
// ─────────────────────────────────────────────────────────────────────────────

// Clear region around an eye
void clearEyeRegion(int16_t cx, int16_t cy, int16_t r=52) {
  gfx->fillRect(cx-r, cy-r-10, r*2, r*2+20, BLACK);
}

// Glowing orb eye (normal/large/wide)
void drawGlowEye(int16_t cx, int16_t cy, int16_t r, int8_t ox, int8_t oy) {
  int16_t px=cx+ox, py=cy+oy;
  gfx->fillCircle(px, py, r+14, ECYAN4);
  gfx->fillCircle(px, py, r+8,  ECYAN3);
  gfx->fillCircle(px, py, r,    ECYAN2);
  gfx->fillCircle(px, py, r-6,  ECYAN);
  gfx->fillCircle(px, py, r-14, EWHITE);
  // shine
  gfx->fillCircle(px+r/3, py-r/3, r/6+1, WHITE);
}

// Squinted eye (excited/cute squint) — bottom arc only
void drawSquintEye(int16_t cx, int16_t cy, int16_t r) {
  for (int x=-r; x<=r; x++) {
    int yy=(int)sqrt(max(0.0f,(float)(r*r-x*x)));
    gfx->fillRect(cx+x, cy, 1, yy+3, ECYAN);
    gfx->fillRect(cx+x, cy, 1, yy+5, ECYAN2);
  }
  // glow above arc
  gfx->drawFastHLine(cx-r, cy-2, r*2, ECYAN3);
}

// Closed eye — horizontal rounded line
void drawClosedEye(int16_t cx, int16_t cy) {
  gfx->fillRoundRect(cx-32, cy-4, 64, 8, 4, ECYAN2);
  gfx->fillRoundRect(cx-28, cy-2, 56, 4, 2, ECYAN);
}

// Droopy / sleepy — top half filled, bottom drooped
void drawSleepyEye(int16_t cx, int16_t cy, int16_t r) {
  // full orb
  gfx->fillCircle(cx, cy, r, ECYAN3);
  gfx->fillCircle(cx, cy, r-6, ECYAN2);
  // cover top 60% with black = droopy look
  gfx->fillRect(cx-r-2, cy-r-2, (r+2)*2, r+8, BLACK);
  // redraw visible bottom arc
  gfx->fillCircle(cx, cy+4, r-4, ECYAN2);
  gfx->fillCircle(cx, cy+4, r-10, ECYAN);
  gfx->fillCircle(cx, cy+4, r-18, EWHITE);
}

// Angry eye — narrow slash
void drawAngryEye(int16_t cx, int16_t cy, int16_t r, bool leftSide) {
  gfx->fillCircle(cx, cy, r, ECYAN3);
  gfx->fillCircle(cx, cy, r-6, ECYAN2);
  // cover with angled black rect
  int slant = leftSide ? -14 : 14;
  gfx->fillTriangle(cx-r-2, cy-r-2,
                    cx+r+2, cy-r-2,
                    cx+(leftSide?r:-r), cy+slant, BLACK);
  gfx->fillCircle(cx, cy+8, r-10, ECYAN);
  gfx->fillCircle(cx, cy+8, r-18, EWHITE);
}

// Heart eye (love mood)
void drawHeartEye(int16_t cx, int16_t cy) {
  int16_t s=22;
  gfx->fillCircle(cx-s/2, cy-s/4, s/2, PINK);
  gfx->fillCircle(cx+s/2, cy-s/4, s/2, PINK);
  gfx->fillTriangle(cx-s, cy, cx+s, cy, cx, cy+s, PINK);
  // glow
  gfx->drawCircle(cx-s/2, cy-s/4, s/2+3, 0xF014);
  gfx->drawCircle(cx+s/2, cy-s/4, s/2+3, 0xF014);
}

// Crescent / looking-side (thinking mood)
void drawCrescentEye(int16_t cx, int16_t cy, int16_t r, int dir) {
  // full orb
  gfx->fillCircle(cx, cy, r+10, ECYAN4);
  gfx->fillCircle(cx, cy, r,    ECYAN2);
  gfx->fillCircle(cx, cy, r-6,  ECYAN);
  // overlay offset circle to create crescent
  gfx->fillCircle(cx+dir*14, cy, r-2, BLACK);
  // thin glow on exposed edge
  for(int a=0;a<360;a+=6) {
    float rad=a*PI/180;
    int px=cx+(int)((r+2)*cos(rad));
    int py=cy+(int)((r+2)*sin(rad));
    if((dir>0&&px<cx)||(dir<0&&px>cx))
      gfx->drawPixel(px,py,ECYAN3);
  }
}

// Tiny dot (boot stage 1)
void drawTinyEye(int16_t cx, int16_t cy) {
  gfx->fillCircle(cx, cy, 8, ECYAN3);
  gfx->fillCircle(cx, cy, 5, ECYAN);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mouth drawing
// ─────────────────────────────────────────────────────────────────────────────
void clearMouthRegion() {
  gfx->fillRect(MX-80, MY-40, 160, 100, BLACK);
}

// Smile arc — thickness t
void drawSmile(int16_t cx, int16_t cy, int16_t r, int16_t depth, uint16_t col, int16_t t=3) {
  for(int x=-r;x<=r;x++) {
    int16_t y=(int16_t)((float)x*x*depth/(r*r));
    for(int tt=0;tt<t;tt++)
      gfx->drawPixel(cx+x, cy+y+tt, col);
  }
}

// Frown (upside-down smile)
void drawFrown(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  for(int x=-r;x<=r;x++) {
    int16_t y=-(int16_t)((float)x*x*14/(r*r))+20;
    gfx->drawPixel(cx+x, cy+y, col);
    gfx->drawPixel(cx+x, cy+y+1, col);
    gfx->drawPixel(cx+x, cy+y+2, col);
  }
}

// Open O mouth (surprised)
void drawOpenMouth(int16_t cx, int16_t cy, int16_t rx, int16_t ry, uint16_t col) {
  gfx->fillEllipse(cx, cy, rx, ry, col);
  gfx->fillEllipse(cx, cy-ry/3, rx-4, ry/3, BLACK);
}

// Flat line (angry/thinking)
void drawFlat(int16_t cx, int16_t cy, int16_t hw, uint16_t col) {
  gfx->fillRoundRect(cx-hw, cy-4, hw*2, 8, 4, col);
}

// Cheeks
void drawCheeks(uint16_t outer, uint16_t inner) {
  gfx->fillCircle(CKL, CKY, 22, outer);
  gfx->fillCircle(CKL, CKY, 13, inner);
  gfx->fillCircle(CKR, CKY, 22, outer);
  gfx->fillCircle(CKR, CKY, 13, inner);
}
void clearCheeks() {
  gfx->fillRect(CKL-28,CKY-28,56,56,BLACK);
  gfx->fillRect(CKR-28,CKY-28,56,56,BLACK);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw eyes by mood
// ─────────────────────────────────────────────────────────────────────────────
void drawEyes() {
  // clear both eye regions
  clearEyeRegion(ELX, EY);
  clearEyeRegion(ERX, EY);

  if(blinking) {
    drawClosedEye(ELX, EY);
    drawClosedEye(ERX, EY);
    return;
  }

  switch(mood) {
    case MOOD_HAPPY:
      drawGlowEye(ELX, EY, 30, eyeOX, eyeOY);
      drawGlowEye(ERX, EY, 24, eyeOX, eyeOY); // asymmetric, right slightly smaller
      break;
    case MOOD_EXCITED:
      drawSquintEye(ELX, EY+10, 30);
      drawSquintEye(ERX, EY+10, 30);
      break;
    case MOOD_CUTE:
      drawGlowEye(ELX, EY, 34, 0, 0);
      drawGlowEye(ERX, EY, 34, 0, 0);
      break;
    case MOOD_THINKING:
      drawCrescentEye(ELX, EY, 28, 1); // looking right
      drawGlowEye(ERX, EY, 26, 4, 0);
      break;
    case MOOD_SLEEPY:
      drawSleepyEye(ELX, EY, 28);
      drawSleepyEye(ERX, EY, 28);
      break;
    case MOOD_SAD:
      drawGlowEye(ELX, EY, 22, 0, 0);
      drawGlowEye(ERX, EY, 22, 0, 0);
      // teardrops
      gfx->fillRect(ELX+4, EY+24, 5, 22, ECYAN3);
      gfx->fillCircle(ELX+6, EY+46, 7, ECYAN2);
      gfx->fillRect(ERX+4, EY+24, 5, 22, ECYAN3);
      gfx->fillCircle(ERX+6, EY+46, 7, ECYAN2);
      break;
    case MOOD_SURPRISED:
      drawGlowEye(ELX, EY, 36, 0, 0);
      drawGlowEye(ERX, EY, 36, 0, 0);
      break;
    case MOOD_ANGRY:
      drawAngryEye(ELX, EY, 28, true);
      drawAngryEye(ERX, EY, 28, false);
      break;
    case MOOD_LOVE:
      drawHeartEye(ELX, EY);
      drawHeartEye(ERX, EY);
      break;
    case MOOD_PARTY:
      drawGlowEye(ELX, EY, 30, eyeOX, eyeOY);
      drawGlowEye(ERX, EY, 30, eyeOX, eyeOY);
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw mouth + cheeks by mood
// ─────────────────────────────────────────────────────────────────────────────
void drawMouth() {
  clearMouthRegion();
  clearCheeks();

  switch(mood) {
    case MOOD_HAPPY:
      drawSmile(MX, MY, 52, 20, WHITE, 4);
      break;
    case MOOD_EXCITED:
      drawSmile(MX, MY-4, 58, 26, WHITE, 5);
      drawCheeks(0xF810, PINK2);
      break;
    case MOOD_CUTE:
      drawSmile(MX, MY, 36, 14, PINK, 4);
      drawCheeks(PINK, PINK2);
      break;
    case MOOD_THINKING:
      drawFlat(MX+16, MY+4, 28, GRAY);
      // thought bubbles top-right
      gfx->fillCircle(ERX+32, EY-28, 5,  DIM);
      gfx->fillCircle(ERX+48, EY-48, 8,  DIM);
      gfx->fillCircle(ERX+62, EY-70, 12, DIM);
      break;
    case MOOD_SLEEPY:
      drawFlat(MX, MY+8, 22, DIM);
      // ZZZ
      gfx->setTextColor(DIM); gfx->setTextSize(2);
      gfx->setCursor(ERX+24, EY-58); gfx->print("z");
      gfx->setTextSize(3);
      gfx->setCursor(ERX+38, EY-82); gfx->print("Z");
      gfx->setTextSize(4);
      gfx->setCursor(ERX+52, EY-114); gfx->print("Z");
      break;
    case MOOD_SAD:
      drawFrown(MX, MY-10, 42, GRAY);
      break;
    case MOOD_SURPRISED:
      drawOpenMouth(MX, MY+6, 18, 26, WHITE);
      break;
    case MOOD_ANGRY:
      drawFlat(MX, MY, 34, RED);
      // angry brows
      gfx->fillRect(ELX-28, EY-52, 56, 7, RED);
      for(int i=0;i<8;i++) gfx->drawPixel(ELX-28+i*2, EY-52-i/2, RED);
      gfx->fillRect(ERX-28, EY-52, 56, 7, RED);
      for(int i=0;i<8;i++) gfx->drawPixel(ERX+28-i*2, EY-52-i/2, RED);
      break;
    case MOOD_LOVE:
      drawSmile(MX, MY, 50, 22, PINK, 4);
      drawCheeks(0xF810, 0xF860);
      // floating hearts
      gfx->setTextColor(PINK); gfx->setTextSize(2);
      gfx->setCursor(ELX-52, EY-80); gfx->print("<3");
      gfx->setCursor(ERX+18, EY-90); gfx->print("<3");
      break;
    case MOOD_PARTY:
      drawSmile(MX, MY-4, 58, 26, YELLOW, 5);
      drawCheeks(0xFDA0, 0xFFE0);
      // confetti
      uint16_t cc[]={YELLOW,PINK,CYAN,GREEN,ORANGE,PURPLE};
      for(int i=0;i<20;i++) {
        int cx2=random(20,W-20), cy2=random(SBAR+4,120);
        gfx->fillCircle(cx2,cy2,4,cc[i%6]);
      }
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Status bar (32px at top)
// ─────────────────────────────────────────────────────────────────────────────
void drawStatusBar() {
  gfx->fillRect(0,0,W,SBAR,BLACK);

  struct tm t;
  if(getLocalTime(&t,30)) {
    char buf[8];
    snprintf(buf,sizeof(buf),"%02d:%02d",t.tm_hour,t.tm_min);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(8,7); gfx->print(buf);
  }

  // WiFi dot
  gfx->fillCircle(W/2, 16, 6, wifiOk ? 0x07E0 : 0xF800);

  // Weather temp top-right
  if(wxOk) {
    String s=wxTemp+"F";
    gfx->setTextColor(CYAN); gfx->setTextSize(2);
    gfx->setCursor(W-(int16_t)s.length()*12-8, 7);
    gfx->print(s);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Boot animation (5 frames matching reference)
// ─────────────────────────────────────────────────────────────────────────────
void bootAnimation() {
  // Frame 1: Power On — tiny dots appear
  gfx->fillScreen(BLACK);
  delay(300);
  gfx->fillCircle(ELX, EY, 6, ECYAN4);
  gfx->fillCircle(ELX, EY, 3, ECYAN3);
  gfx->fillCircle(ERX, EY, 6, ECYAN4);
  gfx->fillCircle(ERX, EY, 3, ECYAN3);
  delay(400);

  // Frame 2: Eyes Open — growing circles
  for(int r=4;r<=30;r+=3) {
    gfx->fillCircle(ELX, EY, r+10, BLACK);
    gfx->fillCircle(ERX, EY, r+10, BLACK);
    gfx->fillCircle(ELX, EY, r+4, ECYAN4);
    gfx->fillCircle(ELX, EY, r,   ECYAN);
    gfx->fillCircle(ERX, EY, r+4, ECYAN4);
    gfx->fillCircle(ERX, EY, r,   ECYAN);
    delay(40);
  }
  delay(200);

  // Frame 3: Happy face
  gfx->fillScreen(BLACK);
  mood=MOOD_HAPPY; blinking=false; eyeOX=0; eyeOY=0;
  drawEyes(); drawMouth();
  delay(600);

  // Frame 4: DIMO name
  gfx->fillScreen(BLACK);
  gfx->setTextColor(CYAN); gfx->setTextSize(5);
  int16_t tw=4*30; // "DIMO" ~4 chars * 30px
  gfx->setCursor((W-tw*2)/2, H/2-40);
  gfx->print("DIMO");
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  centered("desktop friend", H/2+22, DIM, 1);
  delay(800);

  // Frame 5: Ready — happy with cheeks + sparkle dots
  gfx->fillScreen(BLACK);
  drawEyes(); drawMouth();
  // extra cheek pop
  gfx->fillCircle(CKL, CKY, 26, 0xF810);
  gfx->fillCircle(CKL, CKY, 16, PINK2);
  gfx->fillCircle(CKR, CKY, 26, 0xF810);
  gfx->fillCircle(CKR, CKY, 16, PINK2);
  // sparkle stars
  uint16_t sp[]={0xFFFF,CYAN,YELLOW};
  for(int i=0;i<6;i++) {
    int sx=random(30,W-30), sy=random(SBAR+10,H-60);
    gfx->fillCircle(sx,sy,3,sp[i%3]);
    gfx->drawLine(sx-7,sy,sx+7,sy,sp[i%3]);
    gfx->drawLine(sx,sy-7,sx,sy+7,sp[i%3]);
  }
  delay(700);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi connecting animation (5 frames)
// ─────────────────────────────────────────────────────────────────────────────
void wifiAnimation() {
  auto drawWifiIcon = [](int16_t cx, int16_t cy, int bars, uint16_t col) {
    // 3 arcs
    if(bars>=1) gfx->drawCircle(cx,cy,14,col);
    if(bars>=2) gfx->drawCircle(cx,cy,26,col);
    if(bars>=3) gfx->drawCircle(cx,cy,38,col);
    gfx->fillCircle(cx,cy,5,col);
    // mask top half
    gfx->fillRect(cx-44,cy-44,88,44,BLACK);
  };

  // Frame 1: Looking around left
  gfx->fillScreen(BLACK);
  clearEyeRegion(ELX,EY); clearEyeRegion(ERX,EY);
  drawCrescentEye(ELX,EY,28,-1);
  drawCrescentEye(ERX,EY,28,-1);
  drawFlat(MX,MY,28,DIM);
  delay(500);

  // Frame 2: Scanning — eyes looking right + wifi icon
  gfx->fillScreen(BLACK);
  drawCrescentEye(ELX,EY,28,1);
  drawCrescentEye(ERX,EY,28,1);
  drawFlat(MX,MY,28,DIM);
  drawWifiIcon(W/2, H-90, 1, DIM);
  delay(500);

  // Frame 3: Connecting — eyes forward + 2 bars
  gfx->fillScreen(BLACK);
  drawGlowEye(ELX,EY,26,0,0);
  drawGlowEye(ERX,EY,26,0,0);
  drawSmile(MX,MY,36,12,DIM,3);
  drawWifiIcon(W/2, H-90, 2, CYAN);
  delay(500);

  // Frame 4: Almost — eyes up + 3 bars
  gfx->fillScreen(BLACK);
  drawGlowEye(ELX,EY,26,0,-8);
  drawGlowEye(ERX,EY,26,0,-8);
  drawSmile(MX,MY,44,18,CYAN,4);
  drawWifiIcon(W/2, H-90, 3, CYAN);
  delay(400);

  // Frame 5: Connected! — happy + sparkles
  gfx->fillScreen(BLACK);
  mood=MOOD_HAPPY; blinking=false; eyeOX=0; eyeOY=0;
  drawEyes(); drawMouth();
  gfx->setTextColor(GREEN); gfx->setTextSize(1);
  centered("Connected!", H-50, GREEN, 1);
  // sparkle stars around wifi area
  for(int i=0;i<5;i++) {
    int sx=W/2+(i-2)*28, sy=H-80;
    gfx->fillCircle(sx,sy,3,i%2==0?CYAN:GREEN);
  }
  delay(800);
}

// ─────────────────────────────────────────────────────────────────────────────
//  No WiFi animation (offline states)
// ─────────────────────────────────────────────────────────────────────────────
void drawOffline() {
  gfx->fillScreen(BLACK);
  // sad arched brows
  gfx->drawLine(ELX-22,EY-50,ELX+22,EY-42,GRAY);
  gfx->drawLine(ERX-22,EY-42,ERX+22,EY-50,GRAY);
  // eyes
  drawGlowEye(ELX,EY,22,0,0);
  drawGlowEye(ERX,EY,22,0,0);
  drawFrown(MX,MY-10,38,GRAY);
  // wifi X icon
  gfx->drawCircle(W/2,H-90,14,GRAY);
  gfx->drawCircle(W/2,H-90,26,GRAY);
  gfx->fillCircle(W/2,H-90,5,GRAY);
  gfx->fillRect(W/2-44,H-90-44,88,44,BLACK);
  gfx->drawLine(W/2-8,H-80,W/2+8,H-100,RED);
  gfx->drawLine(W/2+8,H-80,W/2-8,H-100,RED);
  gfx->fillCircle(W/2+22,H-70,10,RED);
  gfx->drawLine(W/2+18,H-72,W/2+26,H-68,BLACK);
  gfx->drawLine(W/2+18,H-68,W/2+26,H-72,BLACK);
  centered("No WiFi", H-50, RED, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Clock page
// ─────────────────────────────────────────────────────────────────────────────
void drawClockPage() {
  gfx->fillScreen(BLACK);

  struct tm t;
  if(!getLocalTime(&t,200)) {
    centered("Syncing...", H/2, DIM, 2);
    drawStatusBar();
    return;
  }

  char timeBuf[8], dayBuf[12], dateBuf[14];
  snprintf(timeBuf,sizeof(timeBuf),"%02d:%02d",t.tm_hour,t.tm_min);
  strftime(dayBuf,sizeof(dayBuf),"%A",&t);
  strftime(dateBuf,sizeof(dateBuf),"%b %d",&t);

  // Sleepy DIMO face (small, top area) matching reference "sleeping" state
  int16_t fy=95;
  int16_t felx=ELX, ferx=ERX;
  // small sleepy eyes
  gfx->fillCircle(felx,fy,18,ECYAN3);
  gfx->fillRect(felx-20,fy-20,40,20,BLACK); // droopy
  gfx->fillCircle(felx,fy+4,10,ECYAN2);
  gfx->fillCircle(ferx,fy,18,ECYAN3);
  gfx->fillRect(ferx-20,fy-20,40,20,BLACK);
  gfx->fillCircle(ferx,fy+4,10,ECYAN2);
  // flat mouth
  gfx->fillRoundRect(MX-16,fy+24,32,5,2,DIM);

  // Big time — matching reference (10:30 style)
  gfx->setTextColor(WHITE); gfx->setTextSize(5);
  int16_t tw=(int16_t)strlen(timeBuf)*30;
  gfx->setCursor((W-tw)/2, 170);
  gfx->print(timeBuf);

  // Day + date (FRI 24 MAY style)
  gfx->setTextColor(CYAN); gfx->setTextSize(2);
  centered(dayBuf, 256, CYAN, 2);
  centered(dateBuf, 284, DIM, 1);

  // Seconds dot ring
  gfx->drawCircle(W/2,360,44,DIM);
  float ang=(t.tm_sec/60.0f)*2*PI-PI/2;
  gfx->fillCircle(W/2+(int)(44*cos(ang)),360+(int)(44*sin(ang)),7,CYAN);

  drawStatusBar();
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< face");
  gfx->setCursor(W-66,H-14); gfx->print("weather >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Weather page — matching reference (icon + temp + desc)
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  centered("Cumming, GA", 46, DIM, 1);

  if(!wifiOk) { drawOffline(); return; }
  if(!wxOk)   { centered("Loading...", H/2, DIM, 2); return; }

  // Weather icon (large, centred)
  int16_t ix=W/2, iy=190;
  if(wxDesc.indexOf("clear")>=0||wxDesc.indexOf("sun")>=0) {
    // Sun
    gfx->fillCircle(ix,iy,38,YELLOW);
    for(int a=0;a<360;a+=45) {
      float r=a*PI/180;
      gfx->drawLine(ix+(int)(48*cos(r)),iy+(int)(48*sin(r)),
                    ix+(int)(64*cos(r)),iy+(int)(64*sin(r)),YELLOW);
      gfx->drawLine(ix+(int)(49*cos(r)),iy+(int)(49*sin(r)),
                    ix+(int)(63*cos(r)),iy+(int)(63*sin(r)),YELLOW);
    }
  } else if(wxDesc.indexOf("rain")>=0||wxDesc.indexOf("drizzle")>=0||
            wxDesc.indexOf("storm")>=0||wxDesc.indexOf("thunder")>=0) {
    // Cloud + rain
    gfx->fillEllipse(ix,iy-10,48,28,GRAY);
    gfx->fillEllipse(ix-20,iy-22,30,20,GRAY);
    gfx->fillEllipse(ix+16,iy-24,24,18,GRAY);
    for(int d=-2;d<=2;d++) {
      gfx->fillRoundRect(ix+d*18-3,iy+24,6,22,3,CYAN);
    }
  } else if(wxDesc.indexOf("snow")>=0) {
    // Cloud + snowflakes
    gfx->fillEllipse(ix,iy-10,48,28,WHITE);
    gfx->fillEllipse(ix-20,iy-22,30,20,WHITE);
    for(int d=-2;d<=2;d++) {
      gfx->fillCircle(ix+d*22,iy+34,5,WHITE);
    }
  } else {
    // Cloud
    gfx->fillEllipse(ix,iy,48,28,GRAY);
    gfx->fillEllipse(ix-20,iy-14,30,20,GRAY);
    gfx->fillEllipse(ix+16,iy-16,24,18,GRAY);
  }

  // Temp — big, matching reference
  String ts=wxTemp+" F";
  gfx->setTextColor(WHITE); gfx->setTextSize(4);
  gfx->setCursor((W-(int16_t)ts.length()*24)/2, 280);
  gfx->print(ts);

  // Description
  String dc=wxDesc; if(dc.length()>0) dc[0]=toupper(dc[0]);
  centered(dc.c_str(), 336, CYAN, 2);

  drawStatusBar();
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< clock");
  gfx->setCursor(W-52,H-14); gfx->print("face >");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Music page
// ─────────────────────────────────────────────────────────────────────────────
void drawMusicPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  // Bluetooth icon (top) matching reference bluetooth state
  int16_t bx=W/2, by=80;
  gfx->drawLine(bx,by-26,bx,by+26,CYAN);
  gfx->drawLine(bx,by-26,bx+18,by-10,CYAN);
  gfx->drawLine(bx+18,by-10,bx-14,by+10,CYAN);
  gfx->drawLine(bx-14,by-10,bx+18,by+10,CYAN);
  gfx->drawLine(bx+18,by+10,bx,by+26,CYAN);
  // thick lines
  gfx->drawLine(bx+1,by-26,bx+1,by+26,CYAN);

  // Small neutral face below
  int16_t fy=185;
  gfx->fillCircle(ELX,fy,16,ECYAN3);
  gfx->fillCircle(ELX,fy,10,ECYAN);
  gfx->fillCircle(ERX,fy,16,ECYAN3);
  gfx->fillCircle(ERX,fy,10,ECYAN);
  gfx->fillRoundRect(MX-16,fy+32,32,5,2,GRAY);

  if(bleConn) {
    centered("Connected", 240, GREEN, 1);
  } else {
    centered("Pair: DIMO Remote", 240, DIM, 1);
  }

  // Sound bar visualizer (reference: sound reaction bars)
  int16_t barY=H-130;
  uint8_t bars[]={18,30,44,56,44,30,18};
  for(int i=0;i<7;i++) {
    uint16_t bc=(i==3)?CYAN:ECYAN2;
    gfx->fillRoundRect(W/2-54+i*16, barY-bars[i], 10, bars[i], 3, bc);
  }

  // Controls
  int16_t cy2=H-56;
  // Prev
  gfx->fillTriangle(60,cy2-18,60,cy2+18,40,cy2,WHITE);
  gfx->fillRect(38,cy2-18,6,36,WHITE);
  // Play/Pause
  if(blePlaying) {
    gfx->fillRoundRect(W/2-18,cy2-20,14,40,2,GREEN);
    gfx->fillRoundRect(W/2+4, cy2-20,14,40,2,GREEN);
  } else {
    gfx->fillTriangle(W/2-14,cy2-20,W/2-14,cy2+20,W/2+18,cy2,GREEN);
  }
  // Next
  gfx->fillTriangle(308,cy2-18,308,cy2+18,328,cy2,WHITE);
  gfx->fillRect(324,cy2-18,6,36,WHITE);

  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6,H-14); gfx->print("< face");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Page switcher
// ─────────────────────────────────────────────────────────────────────────────
void showPage(Page p) {
  page=p;
  if(p==PAGE_FACE) { dirtyAll=true; }
  switch(p) {
    case PAGE_FACE:    break; // drawn in loop
    case PAGE_CLOCK:   drawClockPage(); break;
    case PAGE_WEATHER: drawWeatherPage(); break;
    case PAGE_MUSIC:   drawMusicPage(); break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BLE HID (media remote)
// ─────────────────────────────────────────────────────────────────────────────
class DimoBLE : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConn=true;  if(page==PAGE_MUSIC) drawMusicPage(); }
  void onDisconnect(BLEServer*) override { bleConn=false; BLEDevice::startAdvertising(); if(page==PAGE_MUSIC) drawMusicPage(); }
};
void sendKey(uint8_t k) {
  if(!bleIn||!bleConn) return;
  bleIn->setValue(&k,1); bleIn->notify();
  uint8_t r=0; bleIn->setValue(&r,1); bleIn->notify();
}
void setupBLE() {
  BLEDevice::init("DIMO Remote");
  BLEServer *srv=BLEDevice::createServer();
  srv->setCallbacks(new DimoBLE());
  bleHID=new BLEHIDDevice(srv);
  bleIn=bleHID->inputReport(1);
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
//  Touch
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch() {
  int16_t tx,ty;
  bool pressed=touchRead(tx,ty);
  if(pressed) {
    if(!tdDown) {
      tdDown=true; tdSX=tx; tdSY=ty; tdLX=tx; tdLY=ty;
      tdMs=millis(); tdSwiped=false;
    } else { tdLX=tx; tdLY=ty; }
    if(!tdSwiped) {
      int16_t dx=tdLX-tdSX, dy=tdLY-tdSY;
      if(abs(dx)>55&&abs(dx)>abs(dy)*1.4f) {
        tdSwiped=true;
        if(dx<0) showPage((Page)((page+1)%4));
        else     showPage((Page)((page+3)%4));
      }
    }
    if(page==PAGE_MUSIC&&!tdSwiped&&millis()-tdMs<300) {
      if(tx<110)            sendKey(0xB6); // prev
      else if(tx>W-110)     sendKey(0xB5); // next
      else if(abs(tx-W/2)<50) { blePlaying=!blePlaying; sendKey(0xCD); drawMusicPage(); }
    }
  } else { tdDown=false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  USBSerial.begin(115200);
  delay(200);

  Wire.begin(SDA_PIN,SCL_PIN);
  expander.begin(0x20);
  expander.pinMode(4,OUTPUT); expander.pinMode(5,OUTPUT);
  expander.digitalWrite(4,HIGH); expander.digitalWrite(5,HIGH);
  delay(10);

  gfx->begin();
  gfx->setBrightness(230);
  gfx->fillScreen(BLACK);

  // Boot animation
  bootAnimation();

  // WiFi
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info){
    if(ev==ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiOk=true;
      configTzTime(TZ_INFO,"pool.ntp.org","time.nist.gov");
      USBSerial.println("WiFi OK");
    } else if(ev==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiOk=false;
    }
  });
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID,WIFI_PASS);

  // WiFi connecting animation
  wifiAnimation();

  setupBLE();

  dirtyAll=true;
  dirtyEyes=true;
  dirtyMouth=true;
  gfx->fillScreen(BLACK);
  USBSerial.println("DIMO ready");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  uint32_t now=millis();

  handleTouch();

  // ── Face page ──────────────────────────────────────────────────────────────
  if(page==PAGE_FACE) {

    // Full redraw when mood changed
    if(mood!=prevMood) {
      prevMood=mood;
      gfx->fillScreen(BLACK);
      dirtyEyes=true; dirtyMouth=true;
    }

    // Blink
    if(!blinking&&now-lastBlink>(uint32_t)random(3500,7500)) {
      blinking=true; blinkStart=now;
    }
    if(blinking&&now-blinkStart>140) { blinking=false; lastBlink=now; }
    if(blinking!=prevBlink) { prevBlink=blinking; dirtyEyes=true; }

    // Eye wander
    if(now-lastEyeMove>(uint32_t)random(2500,6000)) {
      lastEyeMove=now;
      int8_t nx=random(-7,8), ny=random(-4,5);
      if(nx!=eyeOX||ny!=eyeOY) { eyeOX=nx; eyeOY=ny; dirtyEyes=true; }
    }

    // Auto mood cycle
    if(now-lastMoodChange>moodDur) {
      lastMoodChange=now; moodDur=random(8000,18000);
      Mood all[]={MOOD_HAPPY,MOOD_EXCITED,MOOD_CUTE,MOOD_THINKING,
                  MOOD_SLEEPY,MOOD_SAD,MOOD_SURPRISED,MOOD_LOVE,MOOD_PARTY};
      Mood next=all[random(0,9)];
      if(next!=mood) { mood=next; dirtyEyes=true; dirtyMouth=true; }
    }

    if(dirtyEyes)  { dirtyEyes=false;  drawEyes(); }
    if(dirtyMouth) { dirtyMouth=false; drawMouth(); }

    // Status bar once per second
    static uint32_t lastStat=0;
    if(now-lastStat>1000) { lastStat=now; drawStatusBar(); }
  }

  // ── Clock page ────────────────────────────────────────────────────────────
  static uint32_t lastClk=0;
  if(page==PAGE_CLOCK&&now-lastClk>1000) { lastClk=now; drawClockPage(); }

  // ── WiFi reconnect ────────────────────────────────────────────────────────
  static uint32_t lastWifiTry=0;
  if(!wifiOk&&now-lastWifiTry>12000) {
    lastWifiTry=now; WiFi.begin(WIFI_SSID,WIFI_PASS);
  }

  // ── Weather fetch (on connect, then every 10 min) ─────────────────────────
  if(wifiOk&&!wxFetched) {
    wxFetched=true; lastWxFetch=now;
    HTTPClient http; http.setTimeout(5000);
    if(http.begin(OWM_URL)&&http.GET()==200) {
      String b=http.getString();
      float tmp=jsonF(b,"temp");
      String dsc=jsonS(b,"description");
      if(!isnan(tmp)&&dsc.length()>0) {
        wxTemp=String((int)round(tmp)); wxDesc=dsc; wxOk=true;
      }
    }
    http.end();
    if(page==PAGE_WEATHER) drawWeatherPage(); else drawStatusBar();
  }
  if(wifiOk&&now-lastWxFetch>600000) { lastWxFetch=now; wxFetched=false; }

  delay(18);
}

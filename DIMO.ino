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

// ── Colors ────────────────────────────────────────────────────────────────────
#define BLACK     0x0000
#define WHITE     0xFFFF
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
bool              bleConn    = false;
bool              bleEnabled = true;
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
//  DRAW EYE — called ONCE when face is shown, not in loop
//  Round white eye, dark iris, pupil, two shine dots — exactly like reference
// ─────────────────────────────────────────────────────────────────────────────
void drawEyeFull(int16_t cx, int16_t cy, int8_t gaze = 0) {
  int16_t R = EYE_R; // 44
  gfx->fillCircle(cx, cy, R, WHITE);                    // white sclera
  gfx->fillCircle(cx+gaze, cy, R-12, IRIS_COL);        // iris follows gaze
  gfx->fillCircle(cx+gaze, cy, R-25, BLACK);            // smaller pupil
  gfx->fillCircle(cx+gaze+11, cy-12, 8, WHITE);         // shine follows gaze
  gfx->fillCircle(cx+gaze-7,  cy+10, 4, 0xCF1B);       // secondary shine
}

// ─────────────────────────────────────────────────────────────────────────────
//  BLINK OVERLAY — no redraw, just black rect over eye then restore
//  3 steps: half-close → full-close → open (restore eye underneath)
// ─────────────────────────────────────────────────────────────────────────────
void blinkOverlay(int16_t cx, int16_t cy, int step) {
  // step 1: cover top half
  // step 2: cover full eye (closed line)
  // step 3: restore (redraw eye)
  int16_t R = EYE_R;
  if (step == 1) {
    // Eyelid comes DOWN from top — covers top 60%
    gfx->fillRect(cx-R-1, cy-R-2, (R+1)*2, (int16_t)(R*1.2f), BLACK);
    // Eyelid bottom edge
    gfx->fillRoundRect(cx-R+2, cy-R-2+(int16_t)(R*1.2f)-4, (R-2)*2, 5, 2, LASH_COL);
  } else if (step == 2) {
    // Fully closed — whole eye black + thin line
    gfx->fillCircle(cx, cy, R+1, BLACK);
    gfx->fillRoundRect(cx-R+6, cy-3, (R-6)*2, 7, 3, LASH_COL);
  } else {
    // Open — restore eye with current gaze
    drawEyeFull(cx, cy, gazeDir);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  CHEEKS — drawn once, never redrawn
// ─────────────────────────────────────────────────────────────────────────────
void drawCheeks() {
  // Explicitly erase cheek zones — no cheeks on DIMO
  gfx->fillRect(CK_LX-30, CK_Y-30, 60, 60, BLACK);
  gfx->fillRect(CK_RX-30, CK_Y-30, 60, 60, BLACK);
}

// ─────────────────────────────────────────────────────────────────────────────
//  MOUTH — thick U smile
// ─────────────────────────────────────────────────────────────────────────────
void drawMouth() {
  const int16_t r=34, depth=16;
  for (int x=-r; x<=r; x++) {
    int16_t y = depth - (int16_t)((float)x*x * depth / (r*r));
    gfx->drawPixel(MX+x, MY+y,   WHITE);
    gfx->drawPixel(MX+x, MY+y+1, WHITE);
    gfx->drawPixel(MX+x, MY+y+2, WHITE);
    gfx->drawPixel(MX+x, MY+y+3, WHITE);
    gfx->drawPixel(MX+x, MY+y+4, WHITE);
    gfx->drawPixel(MX+x, MY+y+5, GRAY);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  EYEBROWS — slight outward tilt, friendly expression
// ─────────────────────────────────────────────────────────────────────────────
void drawEyebrows() {
  int16_t by = EY_Y - EYE_R - 16;
  for (int x = -28; x <= 28; x++) {
    // Inner end slightly lower, outer end slightly higher = relaxed/friendly
    int16_t tiltL =  (int16_t)(x * 5 / 28); // left brow
    int16_t tiltR = -(int16_t)(x * 5 / 28); // right brow (mirrored)
    for (int t = 0; t < 5; t++) {
      gfx->drawPixel(EL_X + x, by + tiltL + t, WHITE);
      gfx->drawPixel(ER_X + x, by + tiltR + t, WHITE);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  FULL FACE — draw everything fresh (only on page entry)
// ─────────────────────────────────────────────────────────────────────────────
void drawFacePage() {
  gfx->fillScreen(BLACK);
  drawEyebrows();
  drawEyeFull(EL_X, EY_Y, gazeDir);
  drawEyeFull(ER_X, EY_Y, gazeDir);
  drawCheeks();
  drawMouth();
}

void updateGaze(int8_t newDir) {
  gazeDir = newDir;
  // Erase just the eye circle and redraw — no full screen clear
  gfx->fillCircle(EL_X, EY_Y, EYE_R+1, BLACK);
  gfx->fillCircle(ER_X, EY_Y, EYE_R+1, BLACK);
  drawEyeFull(EL_X, EY_Y, gazeDir);
  drawEyeFull(ER_X, EY_Y, gazeDir);
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
  gfx->setTextColor(WHITE); gfx->setTextSize(6);
  gfx->setCursor((SCR_W-6*36)/2, SCR_H/2-50);
  gfx->print("DIMO");
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  centered("your desktop friend", SCR_H/2+28, DIM, 1);
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
  centered("Connected!", SCR_H-44, GREEN, 1);
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
    centered("Syncing...",SCR_H/2,DIM,2);
    gfx->setTextColor(DIM);gfx->setTextSize(1);
    gfx->setCursor(6,SCR_H-14);gfx->print("< face");
    gfx->setCursor(SCR_W-76,SCR_H-14);gfx->print("weather >");
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
  gfx->setTextColor(WHITE);gfx->setTextSize(5);
  gfx->setCursor((SCR_W-(int16_t)strlen(tBuf)*30)/2,150);
  gfx->print(tBuf);

  gfx->setTextColor(CYAN);gfx->setTextSize(2);
  centered(dBuf,228,CYAN,2);

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

  centered("No WiFi", MY+60, DIM, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WEATHER PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  if (!wifiOk) { drawDeadFace(); return; }

  gfx->fillScreen(BLACK);
  drawStatusBar();
  centered("Cumming, GA",44,DIM,1);

  if (!wxOk)   { centered("Loading...",SCR_H/2,DIM,2); goto hints; }
  if (!wxOk)   { centered("Loading...",SCR_H/2,DIM,2); goto hints; }

  {
    // Big temp
    String ts=wxTemp+" F";
    gfx->setTextColor(WHITE);gfx->setTextSize(5);
    gfx->setCursor((SCR_W-(int16_t)ts.length()*30)/2,80);gfx->print(ts);

    // Description
    String dc=wxDesc; if(dc.length()>0) dc[0]=toupper(dc[0]);
    centered(dc.c_str(),150,CYAN,2);

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

  if(bleConn) centered("Connected",196,GREEN,1);
  else        centered("Pair: DIMO Remote",196,DIM,1);

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
#define SET_ROW_Y1  100   // WiFi row top
#define SET_ROW_Y2  220   // BT row top
#define SET_ROW_H   100

void drawToggle(int16_t x, int16_t y, bool on) {
  uint16_t bg = on ? 0x0400 : GRAY; // green : gray
  gfx->fillRoundRect(x, y, 58, 28, 14, bg);
  int16_t kx = on ? x+34 : x+6;
  gfx->fillCircle(kx+7, y+14, 11, WHITE);
}

void drawSettingsRow(int16_t ry, const char* label, const char* sub, bool on, uint16_t iconCol) {
  gfx->fillRoundRect(12, ry, SCR_W-24, SET_ROW_H-8, 10, 0x18C6); // dark card
  gfx->fillCircle(44, ry+36, 20, iconCol);                         // icon circle
  gfx->setTextColor(WHITE); gfx->setTextSize(2);
  gfx->setCursor(74, ry+20); gfx->print(label);
  gfx->setTextColor(DIM);    gfx->setTextSize(1);
  gfx->setCursor(74, ry+52); gfx->print(sub);
  drawToggle(SCR_W-80, ry+34, on);
}

void drawSettingsPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();

  // Title
  gfx->setTextColor(WHITE); gfx->setTextSize(2);
  centered("SETTINGS", 50, WHITE, 2);
  gfx->drawFastHLine(20, 76, SCR_W-40, DIM);

  // WiFi row
  String wfSub = wifiOk ? String("Connected: ") + WIFI_SSID : (WiFi.status()==WL_NO_SSID_AVAIL ? "Connecting..." : "Disconnected");
  drawSettingsRow(SET_ROW_Y1, "WiFi", wfSub.c_str(), wifiOk, 0x02DF);

  // Bluetooth row
  const char* btSub = bleConn ? "Device connected" : (bleEnabled ? "Visible as DIMO" : "Off");
  drawSettingsRow(SET_ROW_Y2, "Bluetooth", btSub, bleEnabled, 0x001F);

  // Nav hints
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(6, SCR_H-14);        gfx->print("< music");
  gfx->setCursor(SCR_W-72, SCR_H-14); gfx->print("face >");
}

void handleSettingsTap(int16_t y) {
  if (y >= SET_ROW_Y1 && y < SET_ROW_Y1+SET_ROW_H) {
    // WiFi toggle
    if (wifiOk) {
      WiFi.disconnect(); wifiOk=false; wxOk=false; wxFetched=false;
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    drawSettingsPage();
  } else if (y >= SET_ROW_Y2 && y < SET_ROW_Y2+SET_ROW_H) {
    // BLE toggle
    bleEnabled = !bleEnabled;
    if (bleEnabled) BLEDevice::startAdvertising();
    else            BLEDevice::stopAdvertising();
    drawSettingsPage();
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
  gfx->setTextColor(WHITE); gfx->setTextSize(1);
  gfx->setCursor(cx - (int16_t)(strlen(label)*3), cy+ICO_R+10);
  gfx->print(label);
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
  gfx->setTextColor(WHITE); gfx->setTextSize(3);
  gfx->setCursor(20, 38); gfx->print("DIMO");
  gfx->setTextColor(DIM);  gfx->setTextSize(1);
  gfx->setCursor(110, 50); gfx->print("apps");
  gfx->drawFastHLine(0, 72, SCR_W, 0x2104);

  // Row 1
  drawIconBase(ICO_X[0], ICO_Y1, 0x4A49, "Settings"); drawGearIcon(ICO_X[0], ICO_Y1);
  drawIconBase(ICO_X[1], ICO_Y1, 0x0289, "Clock");    drawClockIcon(ICO_X[1], ICO_Y1);
  drawIconBase(ICO_X[2], ICO_Y1, 0x6200, "Weather");  drawSunIcon(ICO_X[2], ICO_Y1);

  // Row 2
  drawIconBase(ICO_X[0], ICO_Y2, 0x0240, "Music");    drawNoteIcon(ICO_X[0], ICO_Y2);

  // Placeholder spots
  gfx->fillCircle(ICO_X[1], ICO_Y2, ICO_R, 0x1082);
  gfx->setTextColor(DIM); gfx->setTextSize(2);
  gfx->setCursor(ICO_X[1]-6, ICO_Y2-8); gfx->print("+");
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  gfx->setCursor(ICO_X[1]-12, ICO_Y2+ICO_R+10); gfx->print("soon");

  gfx->fillCircle(ICO_X[2], ICO_Y2, ICO_R, 0x1082);
  gfx->setCursor(ICO_X[2]-6, ICO_Y2-8); gfx->print("+");
  gfx->setTextSize(1);
  gfx->setCursor(ICO_X[2]-12, ICO_Y2+ICO_R+10); gfx->print("soon");

  // Nav hint
  gfx->setTextColor(DIM); gfx->setTextSize(1);
  centered("swipe up to return", SCR_H-14, DIM, 1);
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
    case PAGE_FACE:     drawFacePage(); drawStatusBar(); blinkState=BLINK_OPEN; blinkTimer=millis(); nextBlink=5000; break;
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
      if(page==PAGE_HOME)     handleHomeTap(tdLX,tdLY);
      if(page==PAGE_SETTINGS) handleSettingsTap(tdLY);
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

  gfx->begin();
  gfx->setBrightness(230);
  gfx->fillScreen(BLACK);

  bootAnimation();

  WiFi.onEvent([](WiFiEvent_t ev,WiFiEventInfo_t info){
    if(ev==ARDUINO_EVENT_WIFI_STA_GOT_IP){wifiOk=true;configTzTime(TZ_INFO,"pool.ntp.org","time.nist.gov");}
    else if(ev==ARDUINO_EVENT_WIFI_STA_DISCONNECTED){wifiOk=false;}
  });
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID,WIFI_PASS);

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

  handleTouch();

  // ── FACE: only blink and sparkle — no other redraws ───────────────────────
  if(page==PAGE_FACE){

    // Gaze animation — look left, center, right
    if (blinkState == BLINK_OPEN && now - gazeTimer > nextGaze) {
      gazeTimer = now;
      nextGaze  = random(3000, 7000);
      static const int8_t dirs[] = {-12, 0, 12, 0};
      static uint8_t gazeIdx = 0;
      gazeIdx = (gazeIdx + 1) % 4;
      updateGaze(dirs[gazeIdx]);
    }

    // Blink state machine — overlay only, no eye redraw
    switch(blinkState){
      case BLINK_OPEN:
        if(now-blinkTimer>nextBlink){
          blinkState=BLINK_CLOSING;
          blinkTimer=now;
        }
        break;
      case BLINK_CLOSING:
        blinkOverlay(EL_X,EY_Y,1); blinkOverlay(ER_X,EY_Y,1);
        blinkState=BLINK_CLOSED;
        blinkTimer=now;
        break;
      case BLINK_CLOSED:
        blinkOverlay(EL_X,EY_Y,2); blinkOverlay(ER_X,EY_Y,2);
        blinkState=BLINK_OPENING;
        blinkTimer=now;
        break;
      case BLINK_OPENING:
        blinkOverlay(EL_X,EY_Y,3); blinkOverlay(ER_X,EY_Y,3);
        blinkState=BLINK_OPEN;
        blinkTimer=now;
        nextBlink=random(3000,7000);
        break;
    }

    // Status bar once per second
    static uint32_t lastStat=0;
    if(now-lastStat>1000){lastStat=now;drawStatusBar();}
  }

  // ── Clock ─────────────────────────────────────────────────────────────────
  static uint32_t lastClk=0;
  if(page==PAGE_CLOCK&&now-lastClk>1000){lastClk=now;drawClockPage();}

  // ── WiFi reconnect ────────────────────────────────────────────────────────
  static uint32_t lastWifiTry=0;
  if(!wifiOk&&now-lastWifiTry>12000){lastWifiTry=now;WiFi.begin(WIFI_SSID,WIFI_PASS);}

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

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
enum Page { PAGE_FACE, PAGE_CLOCK, PAGE_WEATHER, PAGE_MUSIC };
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
  if ((xh&0xC0)==0x80) return false;
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
//  MOUTH — small U smile, drawn once
// ─────────────────────────────────────────────────────────────────────────────
void drawMouth() {
  const int16_t r=30, depth=14;
  for (int x=-r; x<=r; x++) {
    int16_t y = depth - (int16_t)((float)x*x * depth / (r*r));
    gfx->drawPixel(MX+x, MY+y,   IRIS_COL);
    gfx->drawPixel(MX+x, MY+y+1, IRIS_COL);
    gfx->drawPixel(MX+x, MY+y+2, IRIS_COL);
    gfx->drawPixel(MX+x, MY+y+3, IRIS_COL);
    gfx->drawPixel(MX+x, MY+y+4, LASH_COL);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SPARKLE DOTS — small, drawn/erased without clearing eye region
// ─────────────────────────────────────────────────────────────────────────────
// Positions: above-left and above-right of eyes
int16_t spkX[] = {EL_X-38, EL_X-20, ER_X+20, ER_X+38};
int16_t spkY[] = {EY_Y-66, EY_Y-52, EY_Y-52, EY_Y-66};
bool    spkOn  = true;

void drawSparkles(bool on) {
  for (int i=0; i<4; i++) {
    uint16_t col = on ? (i%2==0 ? 0xB5F6 : 0x8C10) : BLACK;
    int16_t  r   = on ? (i%2==0 ? 4 : 3) : 6; // erase slightly larger
    if (!on) {
      gfx->fillRect(spkX[i]-r, spkY[i]-r, r*2+1, r*2+1, BLACK);
    } else {
      gfx->fillCircle(spkX[i], spkY[i], 2, col);
      gfx->drawFastHLine(spkX[i]-5, spkY[i], 11, col);
      gfx->drawFastVLine(spkX[i], spkY[i]-5, 11, col);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  FULL FACE — draw everything fresh (only on page entry)
// ─────────────────────────────────────────────────────────────────────────────
void drawFacePage() {
  gfx->fillScreen(BLACK);
  drawEyeFull(EL_X, EY_Y, gazeDir);
  drawEyeFull(ER_X, EY_Y, gazeDir);
  drawCheeks();
  drawMouth();
  drawSparkles(true);
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
//  WEATHER PAGE
// ─────────────────────────────────────────────────────────────────────────────
void drawWeatherPage() {
  gfx->fillScreen(BLACK);
  drawStatusBar();
  centered("Cumming, GA",44,DIM,1);

  if (!wifiOk) { centered("No WiFi",SCR_H/2,RED_COL,2); goto hints; }
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
}

// ─────────────────────────────────────────────────────────────────────────────
//  Page switcher
// ─────────────────────────────────────────────────────────────────────────────
void showPage(Page p) {
  page = p;
  switch(p){
    case PAGE_FACE:    drawFacePage(); drawStatusBar(); blinkState=BLINK_OPEN; blinkTimer=millis(); nextBlink=5000; break;
    case PAGE_CLOCK:   drawClockPage(); break;
    case PAGE_WEATHER: drawWeatherPage(); break;
    case PAGE_MUSIC:   drawMusicPage(); break;
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
      if(abs(dx)>40 && abs(dx)>abs(dy)){
        tdSwiped=true;
        USBSerial.printf("swipe dx=%d\n",dx);
        if(dx<0) showPage((Page)((page+1)%4));
        else     showPage((Page)((page+3)%4));
      }
    }
    if(page==PAGE_MUSIC&&!tdSwiped&&millis()-tdMs<350){
      if(tx<120)          sendKey(0xB6);
      else if(tx>SCR_W-120) sendKey(0xB5);
      else if(abs(tx-SCR_W/2)<60){blePlaying=!blePlaying;sendKey(0xCD);drawMusicPage();}
    }
  } else { tdDown=false; }
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

    // Sparkle toggle every 800ms — tiny pixels only
    static uint32_t lastSpk=0;
    if(now-lastSpk>800){
      lastSpk=now;
      spkOn=!spkOn;
      drawSparkles(spkOn);
    }

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

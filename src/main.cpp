/*
 * Creality K2 Plus 列印監控 — ESP8266 + ST7789 240x240
 * (支援網頁即時調整螢幕旋轉角度 0, 90, 180, 270 度)
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

// ====================== 可調整 ======================
#define PORTAL_AP_NAME     "K2-Monitor"     // 第一次設定用的 WiFi 熱點名稱
#define MDNS_NAME          "k2-monitor"     // 之後可用 http://k2-monitor.local/ 開設定頁
#define HOST_FILE          "/printer_host.txt"   // 舊版設定檔，只用來遷移
#define CONFIG_FILE        "/config.json"
#define DEFAULT_PORT       7125
#define POLL_DEFAULT_SEC   3                // 資料更新週期預設值（秒），網頁可改 1~30
#define HTTP_TIMEOUT_MS    4000
#define BL_PIN             5                // 背光腳位
#define BL_ON_LEVEL        LOW              // 螢幕全黑的話改成 HIGH
#define FLOW_EFFECT        1                // 外圈流水特效，0 = 關閉
#define FLOW_MS            40               // 流水每幀間隔（毫秒），越小越流暢但越吃 CPU
#define FLOW_STEP          5.0f             // 每幀前進的像素
#define FLOW_LEN           60.0f            // 亮帶長度（像素）
// ====================================================

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define C_BG     RGB565(11, 15, 20)
#define C_TRACK  RGB565(31, 41, 51)
#define C_GREY   RGB565(138, 151, 165)
#define C_BORDER RGB565(58, 69, 82)
#define C_WHITE  RGB565(255, 255, 255)
#define C_GREEN  RGB565(61, 220, 132)
#define C_AMBER  RGB565(255, 176, 32)
#define C_BLUE   RGB565(77, 171, 247)
#define C_RED    RGB565(255, 107, 107)
#define C_ORANGE RGB565(255, 159, 67)
#define C_EMPTY  RGB565(23, 29, 35)

TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);  // 離屏畫布
bool sprOk = false;
bool useSpr = false;                  // 目前繪圖目標：true = sprite，false = 螢幕本身
int ox = 0, oy = 0;                    // 繪圖目標左上角對應的螢幕座標
int curStripY = 0;                    // 目前正在組合的橫帶起始 y（螢幕座標）
ESP8266WebServer web(80);
WiFiManager wm;

String printerHost;            // 例如 "192.168.0.102:7125"
int brightPct = 100;           // 螢幕背光亮度 5~100 (%)
int bgLevel = 70;              // 背景數字亮度 0~255
int pollSec = POLL_DEFAULT_SEC;  // 資料更新週期 1~30 秒
int rotation = 0;              // 螢幕方向：0=0 deg, 1=90 deg, 2=180 deg, 3=270 deg
bool webStarted = false;
unsigned long nextPoll = 0;
unsigned long lastOkMs = 0;
int failCount = 0;
String lastPrinterState = "";

// ====================== 資料 ======================
struct Slot {
  bool has = false;
  uint32_t rgb = 0;
  char mat[8] = "";
};

struct Data {
  String state = "CONNECTING";
  uint16_t stateColor = C_GREY;
  String filename = "";
  float progress = 0;
  int elapsedS = -1, etaS = -1;
  int nozzle = -1, bed = -1;
  Slot slots[4];
  int active = -1;
};

Data cur, shown;
bool shownValid = false;
float shownLen = -1;

// ====================== 設定檔 ======================
void loadConfig() {
  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (f) {
      JsonDocument d;
      if (!deserializeJson(d, f)) {
        printerHost = String((const char *)(d["host"] | ""));
        brightPct = constrain((int)(d["bright"] | 100), 5, 100);
        bgLevel = constrain((int)(d["bg"] | 70), 0, 200);
        pollSec = constrain((int)(d["poll"] | POLL_DEFAULT_SEC), 1, 30);
        rotation = constrain((int)(d["rot"] | 0), 0, 3);
      }
      f.close();
    }
  } else if (LittleFS.exists(HOST_FILE)) {  // 舊版只存了印表機位址
    File f = LittleFS.open(HOST_FILE, "r");
    if (f) {
      printerHost = f.readStringUntil('\n');
      printerHost.trim();
      f.close();
    }
  }
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  JsonDocument d;
  d["host"] = printerHost;
  d["bright"] = brightPct;
  d["bg"] = bgLevel;
  d["poll"] = pollSec;
  d["rot"] = rotation;
  serializeJson(d, f);
  f.close();
}

// 背光用 PWM 調亮度
void setBacklight(int pct) {
  brightPct = constrain(pct, 5, 100);
  int v = brightPct * PWMRANGE / 100;
  analogWrite(BL_PIN, BL_ON_LEVEL == LOW ? PWMRANGE - v : v);
}

// 接受 "192.168.0.102"、"192.168.0.102:7125"、"http://192.168.0.102:7125/"，統一成 "ip:port"
bool normalizeHost(String in, String &out, String &err) {
  in.trim();
  in.replace("http://", "");
  in.replace("https://", "");
  int slash = in.indexOf('/');
  if (slash >= 0) in = in.substring(0, slash);
  in.trim();
  if (in.length() == 0) { err = "請輸入印表機位址"; return false; }
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (!(isAlphaNumeric(c) || c == '.' || c == '-' || c == ':')) {
      err = "位址只能包含數字、字母、點、減號和冒號";
      return false;
    }
  }
  int colon = in.indexOf(':');
  String hostPart = colon >= 0 ? in.substring(0, colon) : in;
  String portPart = colon >= 0 ? in.substring(colon + 1) : "";
  if (hostPart.length() == 0) { err = "缺少 IP 或主機名稱"; return false; }
  long port = DEFAULT_PORT;
  if (portPart.length() > 0) {
    for (unsigned i = 0; i < portPart.length(); i++) {
      if (!isDigit(portPart[i])) { err = "埠號必須是數字"; return false; }
    }
    port = portPart.toInt();
    if (port < 1 || port > 65535) { err = "埠號範圍是 1~65535"; return false; }
  }
  out = hostPart + ":" + String(port);
  return true;
}

// ====================== 外圈進度條 ======================
static const float RING_R = 18.0f;
static const float RING_ARC = RING_R * 1.5707963f;
static const float RING_TOTAL = 92.0f * 2 + 184.0f * 3 + RING_ARC * 4;

static void ringPoint(float s, int &x, int &y) {
  if (s < 0) s = 0;
  if (s > RING_TOTAL) s = RING_TOTAL;
  float fx, fy;
  if (s < 92) { fx = 120 + s; fy = 10; }
  else {
    s -= 92;
    if (s < RING_ARC) { float a = -1.5707963f + s / RING_R; fx = 212 + RING_R * cosf(a); fy = 28 + RING_R * sinf(a); }
    else {
      s -= RING_ARC;
      if (s < 184) { fx = 230; fy = 28 + s; }
      else {
        s -= 184;
        if (s < RING_ARC) { float a = s / RING_R; fx = 212 + RING_R * cosf(a); fy = 212 + RING_R * sinf(a); }
        else {
          s -= RING_ARC;
          if (s < 184) { fx = 212 - s; fy = 230; }
          else {
            s -= 184;
            if (s < RING_ARC) { float a = 1.5707963f + s / RING_R; fx = 28 + RING_R * cosf(a); fy = 212 + RING_R * sinf(a); }
            else {
              s -= RING_ARC;
              if (s < 184) { fx = 10; fy = 212 - s; }
              else {
                s -= 184;
                if (s < RING_ARC) { float a = 3.1415927f + s / RING_R; fx = 28 + RING_R * cosf(a); fy = 28 + RING_R * sinf(a); }
                else { s -= RING_ARC; fx = 28 + s; fy = 10; }
              }
            }
          }
        }
      }
    }
  }
  x = (int)(fx + 0.5f);
  y = (int)(fy + 0.5f);
}

static void drawRing(float from, float to, uint16_t col) {
  if (to <= from) return;
  int x, y;
  for (float s = from; s <= to; s += 3.0f) {
    ringPoint(s, x, y);
    tft.fillCircle(x, y, 8, col);
  }
  ringPoint(to, x, y);
  tft.fillCircle(x, y, 8, col);
}

// ====================== 繪圖輔助 ======================
static void text(const String &s, int x, int y, int font, uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM) {
  tft.setTextDatum(datum);
  tft.setTextColor(fg, bg);
  tft.drawString(s, x, y, font);
}

static String fmtT(int s) {
  if (s < 0) return "--";
  char b[16];
  snprintf(b, sizeof(b), "%dh%02dm", s / 3600, (s % 3600) / 60);
  return String(b);
}

static uint16_t slotColor565(uint32_t rgb) {
  return RGB565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static uint16_t textOn(uint32_t rgb) {
  int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 > 150 ? RGB565(0, 0, 0) : C_WHITE;
}

#define STRIP_X  28
#define STRIP_W  184
#define STRIP_H  25
#define STRIP_Y0 22
#define STRIP_N  8

#define GO(call) do { if (useSpr) spr.call; else tft.call; } while (0)

static inline void gRect(int x, int y, int w, int h, uint16_t c) { GO(fillRect(x - ox, y - oy, w, h, c)); }
static inline void gTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
  GO(fillTriangle(x0 - ox, y0 - oy, x1 - ox, y1 - oy, x2 - ox, y2 - oy, c));
}
static inline void gCircle(int x, int y, int r, uint16_t c) { GO(fillCircle(x - ox, y - oy, r, c)); }
static inline void gCircleO(int x, int y, int r, uint16_t c) { GO(drawCircle(x - ox, y - oy, r, c)); }
static inline void gRRect(int x, int y, int w, int h, int r, uint16_t c) { GO(fillRoundRect(x - ox, y - oy, w, h, r, c)); }
static inline void gRRectO(int x, int y, int w, int h, int r, uint16_t c) { GO(drawRoundRect(x - ox, y - oy, w, h, r, c)); }
static inline void gLine(int x0, int y0, int x1, int y1, uint16_t c) { GO(drawLine(x0 - ox, y0 - oy, x1 - ox, y1 - oy, c)); }
static void gText(const String &s, int x, int y, int font, uint16_t fg, uint8_t datum = TL_DATUM) {
  GO(setTextDatum(datum));
  GO(setTextColor(fg));
  GO(drawString(s, x - ox, y - oy, font));
}

static inline int gTextWidth(const String &s, int font) {
  return useSpr ? spr.textWidth(s, font) : tft.textWidth(s, font);
}

static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {
  int ar = ((a >> 11) & 0x1F) << 3, ag = ((a >> 5) & 0x3F) << 2, ab = (a & 0x1F) << 3;
  int br = ((b >> 11) & 0x1F) << 3, bg = ((b >> 5) & 0x3F) << 2, bb = (b & 0x1F) << 3;
  return RGB565(ar + (br - ar) * t / 255, ag + (bg - ag) * t / 255, ab + (bb - ab) * t / 255);
}

// ---- 七段顯示器背景數字 ----
static const uint8_t SEG7[10] = {
  0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
  0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};

static int bgNum = -1;

static void segH(int x0, int x1, int yc, int t, uint16_t col) {
  gRect(x0 + t / 2 - 1, yc - t / 2, (x1 - x0) - t + 2, t, col);
  gTri(x0, yc, x0 + t / 2, yc - t / 2, x0 + t / 2, yc + t / 2, col);
  gTri(x1, yc, x1 - t / 2, yc - t / 2, x1 - t / 2, yc + t / 2, col);
}

static void segV(int xc, int y0, int y1, int t, uint16_t col) {
  gRect(xc - t / 2, y0 + t / 2 - 1, t, (y1 - y0) - t + 2, col);
  gTri(xc, y0, xc - t / 2, y0 + t / 2, xc + t / 2, y0 + t / 2, col);
  gTri(xc, y1, xc - t / 2, y1 - t / 2, xc + t / 2, y1 - t / 2, col);
}

static void drawSeg7Digit(int d, int x, int y, int w, int h, int t, uint16_t col) {
  const int e = 3;
  int mid = y + h / 2;
  int xl = x + t / 2, xr = x + w - t / 2;
  int yt = y + t / 2, yb = y + h - t / 2;
  uint8_t m = SEG7[d];
  if (m & 0x01) segH(xl + e, xr - e, yt, t, col);
  if (m & 0x02) segV(xr, yt + e, mid - e, t, col);
  if (m & 0x04) segV(xr, mid + e, yb - e, t, col);
  if (m & 0x08) segH(xl + e, xr - e, yb, t, col);
  if (m & 0x10) segV(xl, mid + e, yb - e, t, col);
  if (m & 0x20) segV(xl, yt + e, mid - e, t, col);
  if (m & 0x40) segH(xl + e, xr - e, mid, t, col);
}

static void drawBgNumber() {
  if (bgNum < 0) return;
  uint16_t col = mix565(C_BG, C_WHITE, bgLevel);
  drawSeg7Digit(bgNum / 10, 28, 22, 88, 196, 25, col);
  drawSeg7Digit(bgNum % 10, 124, 22, 88, 196, 25, col);
}

// ====================== 各畫面 ======================
enum Screen { SCR_NONE, SCR_BOOT, SCR_PORTAL, SCR_NOHOST, SCR_WIFILOST, SCR_DASH };
Screen screen = SCR_NONE;

static void drawBoot() {
  tft.fillScreen(C_BG);
  text("K2 Monitor", 120, 80, 4, C_WHITE, C_BG, TC_DATUM);
  text("Connecting WiFi...", 120, 130, 2, C_GREY, C_BG, TC_DATUM);
}

static void drawPortal() {
  tft.fillScreen(C_BG);
  text("WiFi SETUP", 120, 18, 4, C_AMBER, C_BG, TC_DATUM);
  text("1. Join this WiFi:", 20, 64, 2, C_GREY, C_BG);
  text(PORTAL_AP_NAME, 20, 84, 4, C_WHITE, C_BG);
  text("2. Open in browser:", 20, 124, 2, C_GREY, C_BG);
  text("192.168.4.1", 20, 144, 4, C_WHITE, C_BG);
  text("3. Pick your home WiFi", 20, 190, 2, C_GREY, C_BG);
}

static void drawNoHost() {
  tft.fillScreen(C_BG);
  text("WiFi connected", 120, 18, 4, C_GREEN, C_BG, TC_DATUM);
  text("Open in browser:", 20, 72, 2, C_GREY, C_BG);
  text(WiFi.localIP().toString(), 20, 92, 4, C_WHITE, C_BG);
  text("or " MDNS_NAME ".local", 20, 130, 2, C_GREY, C_BG);
  text("Then enter the", 20, 176, 2, C_GREY, C_BG);
  text("printer IP address", 20, 194, 2, C_GREY, C_BG);
}

static void drawWifiLost() {
  tft.fillScreen(C_BG);
  text("WiFi lost", 120, 90, 4, C_RED, C_BG, TC_DATUM);
  text("Reconnecting...", 120, 130, 2, C_GREY, C_BG, TC_DATUM);
}

static void setScreen(Screen s) {
  if (s == screen && shownValid) return;
  screen = s;
  shownValid = false;
  switch (s) {
    case SCR_BOOT:     drawBoot(); break;
    case SCR_PORTAL:   drawPortal(); break;
    case SCR_NOHOST:   drawNoHost(); break;
    case SCR_WIFILOST: drawWifiLost(); break;
    case SCR_DASH:     tft.fillScreen(C_BG); break;
    default: break;
  }
}

static bool slotsEqual(const Data &a, const Data &b) {
  if (a.active != b.active) return false;
  for (int i = 0; i < 4; i++) {
    if (a.slots[i].has != b.slots[i].has || a.slots[i].rgb != b.slots[i].rgb ||
        strcmp(a.slots[i].mat, b.slots[i].mat) != 0) return false;
  }
  return true;
}

static inline bool vis(int y0, int y1) { return y1 > curStripY && y0 < curStripY + STRIP_H; }

static void drawForeground() {
  if (vis(22, 40)) {
    gCircle(34, 31, 5, cur.stateColor);
    gText(cur.state, 44, 23, 2, C_WHITE);
    gText(WiFi.localIP().toString(), 212, 27, 1, C_GREY, TR_DATUM);
  }
  if (vis(40, 56)) {
    String fn = cur.filename;
    bool cut = false;
    while (fn.length() > 1 && gTextWidth(fn + (cut ? ".." : ""), 2) > 184) {
      fn.remove(fn.length() - 1);
      cut = true;
    }
    gText(fn + (cut ? ".." : ""), 28, 40, 2, C_GREY);
  }
  if (vis(66, 112)) {
    gText("ETA", 28, 66, 2, C_GREY);
    gText("ELAPSED", 212, 66, 2, C_GREY, TR_DATUM);
    gText(fmtT(cur.etaS), 28, 84, 4, C_WHITE);
    gText(fmtT(cur.elapsedS), 212, 84, 4, C_WHITE, TR_DATUM);
  }
  if (vis(116, 162)) {
    gText("NOZ", 28, 116, 2, C_ORANGE);
    gText("BED", 212, 116, 2, C_BLUE, TR_DATUM);
    String nv = cur.nozzle < 0 ? String("--") : String(cur.nozzle);
    String bv = cur.bed < 0 ? String("--") : String(cur.bed);
    gText(nv, 28, 134, 4, C_WHITE);
    gText(bv, 204, 134, 4, C_WHITE, TR_DATUM);
    if (cur.nozzle >= 0) gCircleO(28 + gTextWidth(nv, 4) + 5, 139, 3, C_WHITE);
    if (cur.bed >= 0) gCircleO(209, 139, 3, C_WHITE);
  }
  if (vis(176, 222)) {
    for (int i = 0; i < 4; i++) {
      int x = 28 + (i * 146) / 3;
      const int y = 178;
      bool act = (i == cur.active);
      String name = String("1") + (char)('A' + i);
      if (cur.slots[i].has) {
        uint16_t col = slotColor565(cur.slots[i].rgb);
        gRRect(x, y, 38, 22, 5, col);
        if (act) {
          gRRectO(x, y, 38, 22, 5, C_WHITE);
          gRRectO(x + 1, y + 1, 36, 20, 4, C_WHITE);
        } else {
          gRRectO(x, y, 38, 22, 5, C_BORDER);
        }
        gText(name, x + 19, y + 3, 2, textOn(cur.slots[i].rgb), TC_DATUM);
        gText(String(cur.slots[i].mat), x + 19, y + 25, 2, act ? C_WHITE : C_GREY, TC_DATUM);
      } else {
        gRRect(x, y, 38, 22, 5, C_EMPTY);
        gRRectO(x, y, 38, 22, 5, C_BORDER);
        gLine(x + 13, y + 18, x + 25, y + 4, C_GREY);
        gText("--", x + 19, y + 25, 2, C_GREY, TC_DATUM);
      }
    }
  }
}

static void composeStrip(int y0) {
  curStripY = y0;
  if (sprOk) {
    useSpr = true;
    ox = STRIP_X;
    oy = y0;
    spr.fillSprite(C_BG);
    drawBgNumber();
    drawForeground();
    spr.pushSprite(STRIP_X, y0);
  } else {
    useSpr = false;
    ox = 0;
    oy = 0;
    tft.setViewport(STRIP_X, y0, STRIP_W, STRIP_H, false);
    tft.fillRect(STRIP_X, y0, STRIP_W, STRIP_H, C_BG);
    drawBgNumber();
    drawForeground();
    tft.resetViewport();
  }
}

static void renderDash() {
  bool force = !shownValid;
  int pct = (int)(constrain(cur.progress, 0.0f, 1.0f) * 100 + 0.5f);
  if (pct > 99) pct = 99;
  bool showNum = (cur.state == "PRINTING" || cur.state == "PAUSED");
  int num = showNum ? pct : -1;
  static int shownNum = -2;

  float len = constrain(cur.progress, 0.0f, 1.0f) * RING_TOTAL;
  if (force || cur.stateColor != shown.stateColor || len < shownLen - 1) {
    drawRing(0, RING_TOTAL, C_TRACK);
    drawRing(0, len, cur.stateColor);
    shownLen = len;
  } else if (len > shownLen + 1.5f) {
    drawRing(shownLen, len, cur.stateColor);
    shownLen = len;
  }

  uint8_t dirty = 0;
  auto mark = [&](int y0, int y1) {
    for (int k = 0; k < STRIP_N; k++) {
      int s0 = STRIP_Y0 + k * STRIP_H, s1 = s0 + STRIP_H - 1;
      if (y1 >= s0 && y0 <= s1) dirty |= (1 << k);
    }
  };
  if (force || num != shownNum) {
    dirty = 0xFF;
  } else {
    if (cur.state != shown.state) mark(22, 40);
    if (cur.filename != shown.filename) mark(40, 56);
    if (cur.etaS != shown.etaS || cur.elapsedS != shown.elapsedS) mark(66, 112);
    if (cur.nozzle != shown.nozzle || cur.bed != shown.bed) mark(116, 162);
    if (!slotsEqual(cur, shown)) mark(176, 222);
  }
  bgNum = num;
  shownNum = num;
  for (int k = 0; k < STRIP_N; k++)
    if (dirty & (1 << k)) composeStrip(STRIP_Y0 + k * STRIP_H);

  shown = cur;
  shownValid = true;
}

// ====================== 外圈流水特效 ======================
#if FLOW_EFFECT
static float flowHead = 0;
static unsigned long nextFlow = 0;

static uint16_t tint(uint16_t c, uint8_t a) {
  int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
  r += (255 - r) * a / 255;
  g += (255 - g) * a / 255;
  b += (255 - b) * a / 255;
  return RGB565(r, g, b);
}

static void animateFlow() {
  if (!shownValid || cur.state != "PRINTING") return;
  unsigned long now = millis();
  if (now < nextFlow) return;
  nextFlow = now + FLOW_MS;

  float len = shownLen;
  if (len < 14) return;
  float L = FLOW_LEN < len ? FLOW_LEN : len;
  float oldHead = flowHead;
  float newHead = oldHead + FLOW_STEP;

  float eFrom = oldHead - L; if (eFrom < 0) eFrom = 0;
  float eTo = newHead - L;   if (eTo > len) eTo = len;
  if (eTo > eFrom) drawRing(eFrom, eTo, cur.stateColor);

  if (newHead - L > len) { flowHead = 0; return; }

  float a0 = newHead - L; if (a0 < 0) a0 = 0;
  float a1 = newHead;     if (a1 > len) a1 = len;
  int x, y;
  for (float sp = a0; sp <= a1; sp += 3.0f) {
    float t = (sp - (newHead - L)) / L;
    ringPoint(sp, x, y);
    tft.fillCircle(x, y, 8, tint(cur.stateColor, (uint8_t)(40 + 190 * t)));
  }
  flowHead = newHead;
}
#endif

// ====================== Moonraker 輪詢 ======================
static bool parseColor(const char *s, uint32_t &out) {
  if (!s) return false;
  size_t n = strlen(s);
  if (n < 6) return false;
  const char *p = s + n - 6;
  for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)p[i])) return false;
  out = strtoul(p, nullptr, 16);
  return true;
}

static bool pollPrinter(Data &d) {
  if (printerHost.length() == 0) return false;

  String url = "http://" + printerHost +
               "/printer/objects/query?print_stats=state,filename,print_duration"
               "&virtual_sdcard=progress&extruder=temperature&heater_bed=temperature"
               "&box=map,same_material";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  JsonObject fs = filter["result"]["status"].to<JsonObject>();
  fs["print_stats"]["state"] = true;
  fs["print_stats"]["filename"] = true;
  fs["print_stats"]["print_duration"] = true;
  fs["virtual_sdcard"]["progress"] = true;
  fs["extruder"]["temperature"] = true;
  fs["heater_bed"]["temperature"] = true;
  fs["box"]["map"]["T1A"] = true;
  fs["box"]["same_material"][0] = true;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

  JsonObject st = doc["result"]["status"];
  if (st.isNull()) return false;

  String s = st["print_stats"]["state"] | "";
  lastPrinterState = s;
  float prog = st["virtual_sdcard"]["progress"] | 0.0f;
  float dur = st["print_stats"]["print_duration"] | 0.0f;

  d.filename = String((const char *)(st["print_stats"]["filename"] | ""));
  d.nozzle = (int)((float)(st["extruder"]["temperature"] | -1.0f) + 0.5f);
  d.bed = (int)((float)(st["heater_bed"]["temperature"] | -1.0f) + 0.5f);
  if (d.nozzle < 0) d.nozzle = -1;
  if (d.bed < 0) d.bed = -1;

  bool running = (s == "printing" || s == "paused");
  if (s == "printing")       { d.state = "PRINTING";  d.stateColor = C_GREEN; }
  else if (s == "paused")    { d.state = "PAUSED";    d.stateColor = C_AMBER; }
  else if (s == "complete")  { d.state = "COMPLETE";  d.stateColor = C_BLUE; }
  else if (s == "error")     { d.state = "ERROR";     d.stateColor = C_RED; }
  else if (s == "cancelled") { d.state = "CANCELLED"; d.stateColor = C_GREY; }
  else if (s == "standby")   { d.state = "IDLE";      d.stateColor = C_GREY; }
  else { d.state = s.length() ? s : String("UNKNOWN"); d.state.toUpperCase(); d.stateColor = C_GREY; }

  if (s == "complete") prog = 1.0f;
  else if (!running) prog = 0.0f;
  d.progress = constrain(prog, 0.0f, 1.0f);

  if (running) {
    d.elapsedS = (int)dur;
    d.etaS = prog > 0.01f ? (int)(dur / prog - dur) : -1;
  } else {
    d.elapsedS = -1;
    d.etaS = -1;
  }
  if (!running && d.filename.length() == 0) d.filename = "No active job";

  for (int i = 0; i < 4; i++) d.slots[i] = Slot();
  JsonArray sm = st["box"]["same_material"].as<JsonArray>();
  for (JsonVariant ev : sm) {
    JsonArray e = ev.as<JsonArray>();
    const char *colStr = e[1] | "";
    const char *matStr = e[3] | "";
    for (JsonVariant sv : e[2].as<JsonArray>()) {
      const char *code = sv | "";
      if (strlen(code) == 3 && code[0] == 'T' && code[1] == '1' && code[2] >= 'A' && code[2] <= 'D') {
        int i = code[2] - 'A';
        uint32_t rgb;
        if (parseColor(colStr, rgb)) {
          d.slots[i].has = true;
          d.slots[i].rgb = rgb;
          strlcpy(d.slots[i].mat, matStr, sizeof(d.slots[i].mat));
        }
      }
    }
  }

  d.active = -1;
  if (running) {
    const char *m = st["box"]["map"]["T1A"] | "";
    if (strlen(m) == 3 && m[0] == 'T' && m[1] == '1' && m[2] >= 'A' && m[2] <= 'D') d.active = m[2] - 'A';
  }
  return true;
}

static void doPoll() {
  Data n = cur;
  if (pollPrinter(n)) {
    failCount = 0;
    lastOkMs = millis();
    cur = n;
  } else if (++failCount >= 3) {
    cur.state = "OFFLINE";
    cur.stateColor = C_RED;
    cur.etaS = -1;
  }
  renderDash();
}

// ====================== 設定網頁 ======================
static String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-TW"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>K2 Monitor 設定</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:440px;margin:auto;padding:16px}
h1{font-size:20px}.c{background:#1d1d1d;border-radius:12px;padding:14px;margin:12px 0}
label{display:block;color:#9aa;margin-bottom:6px;font-size:14px}
input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;border:1px solid #333;background:#000;color:#fff}
input[type=range]{padding:0;border:0;background:none;height:32px;accent-color:#3ddc84}
b{color:#fff}
button{padding:10px 16px;font-size:15px;border:0;border-radius:8px;background:#3ddc84;color:#000;margin:10px 8px 0 0}
button.g{background:#333;color:#eee}button.r{background:#a33;color:#fff}
td{padding:3px 12px 3px 0;font-size:14px}td:first-child{color:#9aa}
#r{display:block;margin-top:8px;font-size:14px}
</style></head><body><h1>K2 Plus 監控設定</h1>
<div class="c"><form method="POST" action="/save">
<label for="host">印表機位址（IP 或 IP:埠，預設埠 7125）</label>
<input id="host" name="host" placeholder="192.168.0.102:7125" value=")HTML";

static const char PAGE_MID[] PROGMEM = R"HTML(">
<button type="submit">儲存</button><button type="button" class="g" onclick="t()">測試連線</button>
<span id="r"></span></form></div>
<div class="c"><table>)HTML";

static const char PAGE_TAIL1[] PROGMEM = R"HTML(</table></div>)HTML";

static const char PAGE_TAIL2[] PROGMEM = R"HTML(
<div class="c"><form method="POST" action="/reset-wifi" onsubmit="return confirm('確定要清除 WiFi 設定並重新配網？')">
<button class="r" type="submit">重置 WiFi</button></form></div>
<script>
function t(){var h=document.getElementById('host').value,r=document.getElementById('r');r.textContent='測試中…';
fetch('/api/test?host='+encodeURIComponent(h)).then(function(x){return x.json()}).then(function(j){
r.textContent=j.ok?('✔ 連線成功：'+j.name+'（'+j.state+'）'):('✘ '+j.msg)}).catch(function(){r.textContent='✘ 裝置沒回應'})}
var T={};
function bind(id,k,out){var e=document.getElementById(id);
function s(save){if(out)document.getElementById(out).textContent=e.value;clearTimeout(T[id]);
T[id]=setTimeout(function(){fetch('/api/set?'+k+'='+e.value+(save?'&save=1':''))},save?0:100)}
var evt=e.tagName==='SELECT'?'change':'input';
e.addEventListener(evt,function(){s(0)});if(evt!=='change')e.addEventListener('change',function(){s(1)})}
bind('br','bright','bv');bind('bg','bg','gv');bind('pl','poll','pv');bind('rt','rot',null);
</script></body></html>)HTML";

static String row(const String &k, const String &v) {
  return "<tr><td>" + k + "</td><td>" + htmlEscape(v) + "</td></tr>";
}

void handleRoot() {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");
  web.sendContent_P(PAGE_HEAD);
  String hostVal = htmlEscape(printerHost);
  if (hostVal.length() > 0) web.sendContent(hostVal);
  web.sendContent_P(PAGE_MID);
  String ago = lastOkMs ? String((millis() - lastOkMs) / 1000) + " 秒前" : String("尚未成功");
  web.sendContent(row("WiFi", WiFi.SSID()));
  web.sendContent(row("裝置 IP", WiFi.localIP().toString()));
  web.sendContent(row("印表機狀態", lastPrinterState.length() ? lastPrinterState : String("—")));
  web.sendContent(row("上次更新", ago));
  web.sendContent_P(PAGE_TAIL1);
  String sliders = String("<div class=\"c\"><label>螢幕亮度 <b id=\"bv\">") + brightPct + "</b> %</label>"
                   "<input type=\"range\" id=\"br\" min=\"5\" max=\"100\" value=\"" + brightPct + "\">"
                   "<label style=\"margin-top:14px\">背景數字亮度 <b id=\"gv\">" + bgLevel + "</b></label>"
                   "<input type=\"range\" id=\"bg\" min=\"0\" max=\"200\" value=\"" + bgLevel + "\">"
                   "<label style=\"margin-top:14px\">資料更新週期 <b id=\"pv\">" + pollSec + "</b> 秒</label>"
                   "<input type=\"range\" id=\"pl\" min=\"1\" max=\"30\" value=\"" + pollSec + "\">"
                   "<label style=\"margin-top:14px\">畫面旋轉角度</label>"
                   "<select id=\"rt\">"
                   "<option value=\"0\"" + (rotation == 0 ? " selected" : "") + ">0° (標準)</option>"
                   "<option value=\"1\"" + (rotation == 1 ? " selected" : "") + ">90° (順時針)</option>"
                   "<option value=\"2\"" + (rotation == 2 ? " selected" : "") + ">180° (倒立)</option>"
                   "<option value=\"3\"" + (rotation == 3 ? " selected" : "") + ">270° (逆時針)</option>"
                   "</select></div>";
  web.sendContent(sliders);
  web.sendContent_P(PAGE_TAIL2);
  web.sendContent("");
}

void handleSave() {
  String host, err;
  if (!normalizeHost(web.arg("host"), host, err)) {
    web.send(400, "text/plain; charset=utf-8", "格式錯誤：" + err);
    return;
  }
  printerHost = host;
  saveConfig();
  failCount = 0;
  nextPoll = 0;
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleTest() {
  JsonDocument out;
  String host, err;
  if (!normalizeHost(web.arg("host"), host, err)) {
    out["ok"] = false;
    out["msg"] = err;
  } else {
    WiFiClient c;
    HTTPClient http;
    http.setTimeout(3000);
    if (http.begin(c, "http://" + host + "/printer/info") && http.GET() == 200) {
      JsonDocument d;
      if (!deserializeJson(d, http.getString())) {
        out["ok"] = true;
        out["name"] = d["result"]["hostname"] | "?";
        out["state"] = d["result"]["state"] | "?";
      } else {
        out["ok"] = false;
        out["msg"] = "有回應，但不是 Moonraker 的格式";
      }
    } else {
      out["ok"] = false;
      out["msg"] = "連不上 " + host + "（檢查 IP、埠號，或試 4408）";
    }
    http.end();
  }
  String body;
  serializeJson(out, body);
  web.send(200, "application/json; charset=utf-8", body);
}

void handleSet() {
  if (web.hasArg("bright")) setBacklight(web.arg("bright").toInt());
  if (web.hasArg("bg")) {
    bgLevel = constrain((int)web.arg("bg").toInt(), 0, 200);
    if (screen == SCR_DASH) {
      shownValid = false;
      renderDash();
    }
  }
  if (web.hasArg("poll")) {
    pollSec = constrain((int)web.arg("poll").toInt(), 1, 30);
    nextPoll = millis() + (unsigned long)pollSec * 1000UL;
  }
  if (web.hasArg("rot")) {
    rotation = constrain((int)web.arg("rot").toInt(), 0, 3);
    tft.setRotation(rotation);
    shownValid = false;  // 強制重新繪製當前畫面
    setScreen(screen);
    if (screen == SCR_DASH) renderDash();
  }
  if (web.hasArg("save")) saveConfig();
  web.send(200, "text/plain", "ok");
}

void handleInfo() {
  JsonDocument d;
  d["printer"] = printerHost;
  d["ip"] = WiFi.localIP().toString();
  d["state"] = lastPrinterState;
  String body;
  serializeJson(d, body);
  web.send(200, "application/json", body);
}

void handleResetWifi() {
  web.send(200, "text/html; charset=utf-8",
           "<meta charset='utf-8'><body style='font-family:sans-serif'>已清除 WiFi，裝置重啟後請連熱點 " PORTAL_AP_NAME "</body>");
  delay(500);
  wm.resetSettings();
  ESP.restart();
}

void startWeb() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/api/test", HTTP_GET, handleTest);
  web.on("/api/info", HTTP_GET, handleInfo);
  web.on("/api/set", HTTP_GET, handleSet);
  web.on("/reset-wifi", HTTP_POST, handleResetWifi);
  web.begin();
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  webStarted = true;
  Serial.printf("Web: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ====================== setup / loop ======================
void setup() {
  Serial.begin(115200);
  pinMode(BL_PIN, OUTPUT);

  LittleFS.begin();
  loadConfig();
  setBacklight(brightPct);

  tft.init();
  tft.setRotation(rotation);  // 使用設定檔記錄的方向初始化
  sprOk = spr.createSprite(STRIP_W, STRIP_H) != nullptr;
  Serial.printf("sprite %s, free heap %u\n", sprOk ? "ok" : "FAILED", ESP.getFreeHeap());
  tft.fillScreen(C_BG);
  setScreen(SCR_BOOT);

  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(10);
  wm.setAPCallback([](WiFiManager *) { setScreen(SCR_PORTAL); });
  wm.autoConnect(PORTAL_AP_NAME);
}

void loop() {
  wm.process();
  if (webStarted) {
    web.handleClient();
    MDNS.update();
  }

  bool wifiUp = WiFi.status() == WL_CONNECTED;

  if (!webStarted && wifiUp && !wm.getConfigPortalActive()) startWeb();

  if (wifiUp && webStarted) {
    if (printerHost.length() == 0) {
      setScreen(SCR_NOHOST);
    } else {
      if (screen != SCR_DASH) { setScreen(SCR_DASH); }
      if (millis() >= nextPoll) {
        nextPoll = millis() + (unsigned long)pollSec * 1000UL;
        doPoll();
      }
    }
  } else if (wm.getConfigPortalActive()) {
    setScreen(SCR_PORTAL);
  } else if (!wifiUp && webStarted) {
    setScreen(SCR_WIFILOST);
  }
#if FLOW_EFFECT
  if (screen == SCR_DASH) animateFlow();
#endif
  yield();
}

/*
 * Creality K2 Plus 列印監控 — ESP8266 + ST7789 240x240
 *
 * 流程：
 *  1. 第一次開機（或換了 WiFi）→ 螢幕顯示熱點名稱，手機連上後設定 WiFi（WiFiManager）
 *  2. WiFi 連上 → 啟動設定網頁（螢幕顯示裝置 IP），在網頁填「印表機位址」
 *  3. 之後每 3 秒向 Moonraker 要一次資料並更新畫面
 *
 * 未實機編譯測試，請先照 platformio.ini 的註解核對腳位。
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

// ====================== 可調整 ======================
#define PORTAL_AP_NAME     "K2-Monitor"     // 第一次設定用的 WiFi 熱點名稱
#define MDNS_NAME          "k2-monitor"     // 之後可用 http://k2-monitor.local/ 開設定頁
#define HOST_FILE          "/printer_host.txt"
#define DEFAULT_PORT       7125
#define POLL_INTERVAL_MS   3000
#define HTTP_TIMEOUT_MS    4000
#define BL_PIN             5                // 背光腳位
#define BL_ON_LEVEL        LOW              // 螢幕全黑的話改成 HIGH
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
ESP8266WebServer web(80);
WiFiManager wm;

String printerHost;            // 例如 "192.168.0.102:7125"
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
  int layerCur = -1, layerTotal = -1;
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
void loadHost() {
  if (!LittleFS.exists(HOST_FILE)) return;
  File f = LittleFS.open(HOST_FILE, "r");
  if (!f) return;
  printerHost = f.readStringUntil('\n');
  printerHost.trim();
  f.close();
}

void saveHost(const String &h) {
  File f = LittleFS.open(HOST_FILE, "w");
  if (!f) return;
  f.println(h);
  f.close();
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
// 路徑：上緣中間出發、順時針、轉角半徑 18、筆寬 16，貼著 240x240 邊緣
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
  for (float s = from; s <= to; s += 2.0f) {
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

static void drawTemp(bool left, const char *label, uint16_t labelCol, int value, int y) {
  String v = value < 0 ? String("--") : String(value);
  int wl = tft.textWidth(String(label) + " ", 2);
  int wv = tft.textWidth(v, 2);
  int xv = left ? 28 + wl : 206 - wv;
  int xl = left ? 28 : xv - wl;
  text(String(label), xl, y, 2, labelCol, C_BG);
  text(v, xv, y, 2, C_WHITE, C_BG);
  if (value >= 0) tft.drawCircle(xv + wv + 3, y + 3, 2, C_WHITE);
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
  if (s == screen) return;
  screen = s;
  shownValid = false;
  switch (s) {
    case SCR_BOOT:     drawBoot(); break;
    case SCR_PORTAL:   drawPortal(); break;
    case SCR_NOHOST:   drawNoHost(); break;
    case SCR_WIFILOST: drawWifiLost(); break;
    case SCR_DASH:     tft.fillScreen(C_BG); break;  // 內容由 renderDash 補畫
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

static void renderDash() {
  bool force = !shownValid;

  // 外圈進度條
  float len = constrain(cur.progress, 0.0f, 1.0f) * RING_TOTAL;
  if (force || cur.stateColor != shown.stateColor || len < shownLen - 1) {
    drawRing(0, RING_TOTAL, C_TRACK);
    drawRing(0, len, cur.stateColor);
    shownLen = len;
  } else if (len > shownLen + 1.5f) {
    drawRing(shownLen, len, cur.stateColor);
    shownLen = len;
  }

  // 狀態列：圓點 + 狀態 + 層數
  if (force || cur.state != shown.state || cur.stateColor != shown.stateColor ||
      cur.layerCur != shown.layerCur || cur.layerTotal != shown.layerTotal) {
    tft.fillRect(28, 22, 184, 18, C_BG);
    tft.fillCircle(34, 31, 5, cur.stateColor);
    text(cur.state, 44, 23, 2, C_WHITE, C_BG);
    if (cur.layerCur >= 0) {
      String ly = "L" + String(cur.layerCur) + (cur.layerTotal > 0 ? "/" + String(cur.layerTotal) : "");
      text(ly, 212, 23, 2, C_GREY, C_BG, TR_DATUM);
    }
  }

  // 檔名（太長就截斷加 ..）
  if (force || cur.filename != shown.filename) {
    tft.fillRect(28, 40, 184, 16, C_BG);
    String fn = cur.filename;
    bool cut = false;
    while (fn.length() > 1 && tft.textWidth(fn + (cut ? ".." : ""), 2) > 184) {
      fn.remove(fn.length() - 1);
      cut = true;
    }
    text(fn + (cut ? ".." : ""), 28, 40, 2, C_GREY, C_BG);
  }

  // 百分比
  int pct = (int)(constrain(cur.progress, 0.0f, 1.0f) * 100 + 0.5f);
  int shownPct = (int)(constrain(shown.progress, 0.0f, 1.0f) * 100 + 0.5f);
  if (force || pct != shownPct) {
    tft.fillRect(28, 58, 184, 50, C_BG);
    String n = String(pct);
    int wn = tft.textWidth(n, 6);
    int wp = tft.textWidth("%", 4);
    int x0 = 120 - (wn + wp + 2) / 2;
    text(n, x0, 58, 6, C_WHITE, C_BG);
    text("%", x0 + wn + 2, 80, 4, C_GREY, C_BG);
  }

  // ETA / ELAPSED
  if (force || cur.etaS != shown.etaS || cur.elapsedS != shown.elapsedS) {
    tft.fillRect(28, 110, 184, 44, C_BG);
    text("ETA", 28, 110, 2, C_GREY, C_BG);
    text("ELAPSED", 212, 110, 2, C_GREY, C_BG, TR_DATUM);
    text(fmtT(cur.etaS), 28, 126, 4, C_WHITE, C_BG);
    text(fmtT(cur.elapsedS), 212, 126, 4, C_WHITE, C_BG, TR_DATUM);
  }

  // 溫度
  if (force || cur.nozzle != shown.nozzle || cur.bed != shown.bed) {
    tft.fillRect(28, 156, 184, 18, C_BG);
    drawTemp(true, "NOZ", C_ORANGE, cur.nozzle, 158);
    drawTemp(false, "BED", C_BLUE, cur.bed, 158);
  }

  // 四個卡槽 + 材質，使用中的用白色粗框
  if (force || !slotsEqual(cur, shown)) {
    tft.fillRect(28, 176, 184, 44, C_BG);
    for (int i = 0; i < 4; i++) {
      int x = 28 + (i * 146) / 3;
      const int y = 178;
      bool act = (i == cur.active);
      String name = String("1") + (char)('A' + i);
      if (cur.slots[i].has) {
        uint16_t col = slotColor565(cur.slots[i].rgb);
        tft.fillRoundRect(x, y, 38, 22, 5, col);
        if (act) {
          tft.drawRoundRect(x, y, 38, 22, 5, C_WHITE);
          tft.drawRoundRect(x + 1, y + 1, 36, 20, 4, C_WHITE);
        } else {
          tft.drawRoundRect(x, y, 38, 22, 5, C_BORDER);
        }
        text(name, x + 19, y + 3, 2, textOn(cur.slots[i].rgb), col, TC_DATUM);
        text(String(cur.slots[i].mat), x + 19, y + 25, 2, act ? C_WHITE : C_GREY, C_BG, TC_DATUM);
      } else {
        tft.fillRoundRect(x, y, 38, 22, 5, C_EMPTY);
        tft.drawRoundRect(x, y, 38, 22, 5, C_BORDER);
        tft.drawLine(x + 13, y + 18, x + 25, y + 4, C_GREY);
        text("--", x + 19, y + 25, 2, C_GREY, C_BG, TC_DATUM);
      }
    }
  }

  shown = cur;
  shownValid = true;
}

// ====================== Moonraker 輪詢 ======================
static bool parseColor(const char *s, uint32_t &out) {
  if (!s) return false;
  size_t n = strlen(s);
  if (n < 6) return false;
  const char *p = s + n - 6;  // 取最後 6 碼（"01b04ae" → "1b04ae"）
  for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)p[i])) return false;
  out = strtoul(p, nullptr, 16);
  return true;
}

static bool pollPrinter(Data &d) {
  if (printerHost.length() == 0) return false;

  String url = "http://" + printerHost +
               "/printer/objects/query?print_stats=state,filename,print_duration,info"
               "&virtual_sdcard=progress&extruder=temperature&heater_bed=temperature"
               "&box=map,same_material";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);  // 避免 chunked，讓 ArduinoJson 直接讀串流
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  JsonObject fs = filter["result"]["status"].to<JsonObject>();
  fs["print_stats"]["state"] = true;
  fs["print_stats"]["filename"] = true;
  fs["print_stats"]["print_duration"] = true;
  fs["print_stats"]["info"]["current_layer"] = true;
  fs["print_stats"]["info"]["total_layer"] = true;
  fs["virtual_sdcard"]["progress"] = true;
  fs["extruder"]["temperature"] = true;
  fs["heater_bed"]["temperature"] = true;
  fs["box"]["map"]["T1A"] = true;
  fs["box"]["same_material"][0] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

  JsonObject st = doc["result"]["status"];
  if (st.isNull()) return false;

  String s = st["print_stats"]["state"] | "";
  lastPrinterState = s;
  float prog = st["virtual_sdcard"]["progress"] | 0.0f;
  float dur = st["print_stats"]["print_duration"] | 0.0f;

  d.filename = String((const char *)(st["print_stats"]["filename"] | ""));
  d.layerCur = st["print_stats"]["info"]["current_layer"] | -1;
  d.layerTotal = st["print_stats"]["info"]["total_layer"] | -1;
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
  if (!running) { d.layerCur = -1; d.layerTotal = -1; }
  if (!running && d.filename.length() == 0) d.filename = "No active job";

  // 卡槽：顏色與材質來自 same_material，格式 [材質碼, 顏色, [槽位...], 材質名]
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

  // 使用中的槽：看列印檔的 T1A 被對應到哪一槽（單色列印驗證過；多色換槽邏輯尚未驗證）
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
input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;border:1px solid #333;background:#000;color:#fff}
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

static const char PAGE_TAIL[] PROGMEM = R"HTML(</table></div>
<div class="c"><form method="POST" action="/reset-wifi" onsubmit="return confirm('確定要清除 WiFi 設定並重新配網？')">
<button class="r" type="submit">重置 WiFi</button></form></div>
<script>
function t(){var h=document.getElementById('host').value,r=document.getElementById('r');r.textContent='測試中…';
fetch('/api/test?host='+encodeURIComponent(h)).then(function(x){return x.json()}).then(function(j){
r.textContent=j.ok?('✔ 連線成功：'+j.name+'（'+j.state+'）'):('✘ '+j.msg)}).catch(function(){r.textContent='✘ 裝置沒回應'})}
</script></body></html>)HTML";

static String row(const String &k, const String &v) {
  return "<tr><td>" + k + "</td><td>" + htmlEscape(v) + "</td></tr>";
}

void handleRoot() {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");
  web.sendContent_P(PAGE_HEAD); // 注意：chunked 模式下 sendContent("") 會直接結束回應，所以空字串不能送
String hostVal = htmlEscape(printerHost);
if (hostVal.length() > 0) web.sendContent(hostVal);
  
  web.sendContent_P(PAGE_MID);
  String ago = lastOkMs ? String((millis() - lastOkMs) / 1000) + " 秒前" : String("尚未成功");
  web.sendContent(row("WiFi", WiFi.SSID()));
  web.sendContent(row("裝置 IP", WiFi.localIP().toString()));
  web.sendContent(row("印表機狀態", lastPrinterState.length() ? lastPrinterState : String("—")));
  web.sendContent(row("上次更新", ago));
  web.sendContent_P(PAGE_TAIL);
  web.sendContent("");
}

void handleSave() {
  String host, err;
  if (!normalizeHost(web.arg("host"), host, err)) {
    web.send(400, "text/plain; charset=utf-8", "格式錯誤：" + err);
    return;
  }
  printerHost = host;
  saveHost(host);
  failCount = 0;
  nextPoll = 0;  // 立刻重新輪詢
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
  digitalWrite(BL_PIN, BL_ON_LEVEL);

  LittleFS.begin();
  loadHost();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  setScreen(SCR_BOOT);

  // 非阻塞配網：沒存過 WiFi 或連不上時，開熱點讓手機設定
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

  // 配網入口和自己的網頁伺服器都用 80 埠：等 WiFi 連上、入口結束後才啟動
  if (!webStarted && wifiUp && !wm.getConfigPortalActive()) startWeb();

  if (wifiUp && webStarted) {
    if (printerHost.length() == 0) {
      setScreen(SCR_NOHOST);
    } else {
      if (screen != SCR_DASH) { setScreen(SCR_DASH); }
      if (millis() >= nextPoll) {
        nextPoll = millis() + POLL_INTERVAL_MS;
        doPoll();
      }
    }
  } else if (wm.getConfigPortalActive()) {
    setScreen(SCR_PORTAL);
  } else if (!wifiUp && webStarted) {
    setScreen(SCR_WIFILOST);
  }
  yield();
}

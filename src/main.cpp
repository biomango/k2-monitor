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

// 精簡 PNG 縮圖解碼器：給 ESP8266 這類記憶體很小的裝置用。
// 重點：deflate 的「回溯視窗」(最多 32KB) 不放在 RAM，而是用 2KB 環形緩衝 + 檔案系統暫存檔，
// 所以只需要約 5KB 的 RAM 就能解 96x96 RGBA 的縮圖。
// 只支援：8 位元、非交錯、RGB(2) 或 RGBA(6) 的 PNG。輸出為縮放後的 RGB565（小端），透明像素 = 0x0001。
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define TD_RING     2048      // RAM 環形緩衝（必須是 2 的次方）
#define TD_BLK      512       // 每滿 512 位元組寫一次到暫存檔（必須整除 TD_RING）
#define TD_MAXW     128       // 支援的最大原圖寬度
#define TD_TRANSP   0x0001    // 輸出檔裡代表「透明」的像素值

template <class Store>
class ThumbDecoder {
 public:
  explicit ThumbDecoder(Store &s) : st(s) {}

  // png: 完整的 PNG 檔案內容；outW x outH: 輸出尺寸（最近鄰縮放）
  bool decode(const uint8_t *png, size_t len, int outW, int outH) {
    ow = outW; oh = outH;
    if (len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    // 掃描 chunk：讀 IHDR、記下所有 IDAT 的位置
    size_t pos = 8; nIdat = 0; bool gotHdr = false;
    while (pos + 12 <= len) {
      uint32_t cl = be32(png + pos);
      const uint8_t *ty = png + pos + 4;
      if (pos + 12 + cl > len) return false;
      if (!memcmp(ty, "IHDR", 4)) {
        sw = be32(png + pos + 8); sh = be32(png + pos + 12);
        uint8_t bd = png[pos + 16], ct = png[pos + 17], il = png[pos + 20];
        if (bd != 8 || il != 0 || (ct != 6 && ct != 2)) return false;
        bpp = (ct == 6) ? 4 : 3;
        if (sw == 0 || sw > TD_MAXW || sh == 0) return false;
        gotHdr = true;
      } else if (!memcmp(ty, "IDAT", 4)) {
        if (nIdat >= 8) return false;
        idatPos[nIdat] = pos + 8; idatLen[nIdat] = cl; nIdat++;
      } else if (!memcmp(ty, "IEND", 4)) break;
      pos += 12 + cl;
    }
    if (!gotHdr || nIdat == 0) return false;
    src = png; curChunk = 0; curOff = 0;
    stride = 1 + sw * bpp;
    wpos = 0; rowPos = 0; rowsDone = 0; nextDy = 0; bitCount = 0; tag = 0; err = false;
    memset(prev, 0, sizeof(prev));
    // zlib 標頭 2 位元組
    nextByte(); nextByte();
    if (err) return false;
    if (!inflate()) return false;
    return !err && rowsDone == sh && nextDy == (uint32_t)oh;
  }

 private:
  struct Tree { uint16_t counts[16]; uint16_t syms[288]; };

  Store &st;
  int ow = 0, oh = 0;
  uint32_t sw = 0, sh = 0; int bpp = 4; uint32_t stride = 0;
  const uint8_t *src = nullptr;
  size_t idatPos[8], idatLen[8]; int nIdat = 0; int curChunk = 0; size_t curOff = 0;
  uint8_t tag = 0; int bitCount = 0; bool err = false;
  uint32_t wpos = 0;
  uint8_t ring[TD_RING];
  uint8_t cur[1 + TD_MAXW * 4];
  uint8_t prev[TD_MAXW * 4];
  uint32_t rowPos = 0, rowsDone = 0, nextDy = 0;
  Tree lt, dt, ct;
  uint8_t lens[320];
  uint8_t lineBuf[TD_MAXW * 2];
  uint8_t tmpBuf[64];

  static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

  // ---------- 輸入：把所有 IDAT 串起來當成一條位元組流 ----------
  int nextByte() {
    while (curChunk < nIdat && curOff >= idatLen[curChunk]) { curChunk++; curOff = 0; }
    if (curChunk >= nIdat) { err = true; return 0; }
    return src[idatPos[curChunk] + curOff++];
  }
  int getbit() {
    if (!bitCount) { tag = (uint8_t)nextByte(); bitCount = 8; }
    int b = tag & 1; tag >>= 1; bitCount--; return b;
  }
  uint32_t readBits(int n, uint32_t base) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) if (getbit()) v |= (1u << i);
    return v + base;
  }

  // ---------- Huffman ----------
  static void build(Tree &t, const uint8_t *lens, int n) {
    uint16_t offs[16]; memset(t.counts, 0, sizeof(t.counts));
    for (int i = 0; i < n; i++) t.counts[lens[i]]++;
    t.counts[0] = 0;
    uint16_t sum = 0;
    for (int i = 0; i < 16; i++) { offs[i] = sum; sum += t.counts[i]; }
    for (int i = 0; i < n; i++) if (lens[i]) t.syms[offs[lens[i]]++] = i;
  }
  int decodeSym(const Tree &t) {
    int sum = 0, c = 0, l = 0;
    do {
      c = 2 * c + getbit(); ++l;
      if (l > 15) { err = true; return 0; }
      sum += t.counts[l]; c -= t.counts[l];
    } while (c >= 0);
    return t.syms[sum + c];
  }

  // ---------- 輸出：環形緩衝 + 暫存檔 ----------
  void emit(uint8_t b) {
    ring[wpos & (TD_RING - 1)] = b; wpos++;
    if ((wpos & (TD_BLK - 1)) == 0) {
      uint32_t s = wpos - TD_BLK;
      if (!st.winWrite(s, ring + (s & (TD_RING - 1)), TD_BLK)) err = true;
    }
    cur[rowPos++] = b;
    if (rowPos == stride) { processRow(); rowPos = 0; rowsDone++; }
  }
  bool copyMatch(int len, uint32_t dist) {
    if (dist == 0 || dist > wpos) return false;
    while (len > 0 && !err) {
      if (dist <= TD_RING) { emit(ring[(wpos - dist) & (TD_RING - 1)]); len--; }
      else {                                        // 比 2KB 更遠：從暫存檔讀（這段一定已經寫入）
        int n = len < 64 ? len : 64;
        if (!st.winRead(wpos - dist, tmpBuf, n)) { err = true; return false; }
        for (int i = 0; i < n; i++) emit(tmpBuf[i]);
        len -= n;
      }
    }
    return !err;
  }

  // ---------- PNG 逐列處理：反濾波 → 縮放 → 輸出 ----------
  void processRow() {
    uint8_t ft = cur[0]; uint8_t *px = cur + 1; int n = sw * bpp;
    for (int i = 0; i < n; i++) {
      int a = i >= bpp ? px[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = px[i], r;
      switch (ft) {
        case 0: r = x; break;
        case 1: r = x + a; break;
        case 2: r = x + b; break;
        case 3: r = x + ((a + b) >> 1); break;
        case 4: { int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                  r = x + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)); break; }
        default: err = true; return;
      }
      px[i] = (uint8_t)r;
    }
    memcpy(prev, px, n);
    st.tick();
    uint32_t sy = rowsDone;
    while ((int)nextDy < oh && (nextDy * sh) / oh == sy) { writeDestRow(px, nextDy); nextDy++; }
  }
  void writeDestRow(const uint8_t *px, uint32_t y) {
    uint8_t *line = lineBuf;
    for (int x = 0; x < ow && x < TD_MAXW; x++) {
      const uint8_t *p = px + ((uint32_t)x * sw / ow) * bpp;
      uint16_t v;
      if (bpp == 4 && p[3] < 128) v = TD_TRANSP;
      else { v = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3); if (v == TD_TRANSP) v = 0x0002; }
      line[x * 2] = v & 0xFF; line[x * 2 + 1] = v >> 8;
    }
    if (!st.outWrite(y * ow * 2, line, ow * 2)) err = true;
  }

  // ---------- deflate ----------
  bool inflate() {
    static const uint16_t LB[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t  LE[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t DB[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t  DE[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int final;
    do {
      final = getbit();
      int type = (int)readBits(2, 0);
      if (err) return false;
      if (type == 1) {
        int i = 0;
        for (; i < 144; i++) lens[i] = 8;
        for (; i < 256; i++) lens[i] = 9;
        for (; i < 280; i++) lens[i] = 7;
        for (; i < 288; i++) lens[i] = 8;
        build(lt, lens, 288);
        for (i = 0; i < 30; i++) lens[i] = 5;
        build(dt, lens, 30);
      } else if (type == 2) {
        static const uint8_t ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        int hlit = readBits(5, 257), hdist = readBits(5, 1), hclen = readBits(4, 4);
        memset(lens, 0, sizeof(lens));
        for (int i = 0; i < hclen; i++) lens[ORD[i]] = (uint8_t)readBits(3, 0);
        build(ct, lens, 19);
        int num = 0;
        while (num < hlit + hdist) {
          int sym = decodeSym(ct); if (err) return false;
          int rep, v = 0;
          if (sym < 16) { lens[num++] = (uint8_t)sym; continue; }
          if (sym == 16) { if (!num) return false; v = lens[num - 1]; rep = readBits(2, 3); }
          else if (sym == 17) rep = readBits(3, 3);
          else rep = readBits(7, 11);
          if (num + rep > hlit + hdist) return false;
          while (rep--) lens[num++] = (uint8_t)v;
        }
        build(lt, lens, hlit);
        build(dt, lens + hlit, hdist);
      } else return false;      // 不壓縮區塊(0)這裡不支援

      for (;;) {
        int sym = decodeSym(lt); if (err) return false;
        if (sym < 256) emit((uint8_t)sym);
        else if (sym == 256) break;
        else {
          sym -= 257; if (sym >= 29) return false;
          int ln = readBits(LE[sym], LB[sym]);
          int ds = decodeSym(dt); if (err || ds >= 30) return false;
          uint32_t dist = readBits(DE[ds], DB[ds]);
          if (!copyMatch(ln, dist)) return false;
        }
        if (err) return false;
      }
    } while (!final);
    return true;
  }
};


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
#define BL_PWM_RANGE       1023             // 背光 PWM 解析度（setup 會明確設定，不依賴核心預設值）
#define FLOW_EFFECT        1                // 外圈流水特效，0 = 關閉
#define FLOW_MS            40               // 流水每幀間隔（毫秒），越小越流暢但越吃 CPU
#define FLOW_STEP          5.0f             // 每幀前進的像素
#define FLOW_LEN           60.0f            // 亮帶長度（像素）
// ====================================================

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define C_BG     RGB565(18, 21, 43)      // 深藍黑（跟印表機螢幕同風格）
#define C_TRACK  RGB565(38, 43, 85)
#define C_GREY   RGB565(170, 178, 240)   // 淡紫灰：次要文字
#define C_BORDER RGB565(90, 96, 148)
#define C_WHITE  RGB565(255, 255, 255)
#define C_GREEN  RGB565(43, 212, 125)
#define C_AMBER  RGB565(255, 176, 32)
#define C_BLUE   RGB565(125, 180, 255)
#define C_RED    RGB565(255, 107, 107)
#define C_ORANGE RGB565(255, 171, 77)
#define C_EMPTY  RGB565(28, 32, 64)
#define C_CARD   RGB565(112, 120, 205)   // 半透明卡片的混色
#define C_CARDSOLID RGB565(46, 51, 92)   // 沒有 sprite 時的備援：卡片實心色
#define CARD_A5  10                      // 卡片不透明度 0~32（10 ≈ 30%）

TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);  // 離屏畫布：每次畫一條 184x25 的橫帶，畫好再一次推上螢幕，避免「先清掉再重畫」造成的閃爍
bool sprOk = false;
bool useSpr = false;                  // 目前繪圖目標：true = sprite，false = 螢幕本身
int ox = 0, oy = 0;                   // 繪圖目標左上角對應的螢幕座標
int curStripY = 0;                    // 目前正在組合的橫帶起始 y（螢幕座標）
ESP8266WebServer web(80);
WiFiManager wm;

String printerHost;            // 例如 "192.168.0.102:7125"
int brightPct = 100;           // 螢幕背光亮度 5~100 (%)
int bgLevel = 70;              // 背景數字亮度 0~255
int pollSec = POLL_DEFAULT_SEC;  // 資料更新週期 1~30 秒
int rotation = 0;              // 畫面旋轉 0~3（每格 90°）

// ---- 列印檔縮圖 ----
#define THUMB_W    84
#define THUMB_H    84
#define THUMB_FILE "/thumb.bin"
String thumbName = "";         // 最近一次處理過（成功或失敗）的檔名，同一個檔案只抓一次
bool thumbOk = false;
int thumbVer = 0;              // 縮圖有變動就加 1，畫面會跟著重畫
File tf;                       // 畫圖時用的唯讀檔案
static void closeThumbFile() { if (tf) tf.close(); }
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

// 背光用 PWM 調亮度；BL_ON_LEVEL 為 LOW 的板子是「低電位亮」，所以佔空比要反過來
void setBacklight(int pct) {
  brightPct = constrain(pct, 5, 100);
  int v = brightPct * BL_PWM_RANGE / 100;
  analogWrite(BL_PIN, BL_ON_LEVEL == LOW ? BL_PWM_RANGE - v : v);
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

// 卡片用的剩餘時間：H:MM（例如 0:30、3:12），寬度跟溫度數字差不多，才能跟溫度一樣用大字型；
// 10 小時以上只顯示小時（例如 12h），不然 "12:30" 會超過卡片寬度，被迫改用小字型
static String fmtEta(int s) {
  if (s < 0) return "--";
  char b[16];
  int h = s / 3600, m = (s % 3600) / 60;
  if (h >= 10) snprintf(b, sizeof(b), "%dh", h);
  else snprintf(b, sizeof(b), "%d:%02d", h, m);
  return String(b);
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

// ---- 離屏繪圖包裝：呼叫時一律用「螢幕座標」，自動換算成目前繪圖目標（sprite 或螢幕）的座標 ----
#define STRIP_X  28
#define STRIP_W  184
#define STRIP_H  25
#define STRIP_Y0 22
#define STRIP_N  8   // 8 條 x 25 列 = y 22~221

// 直接對 sprite 或 tft 這個物件呼叫（不經過基底類別指標），確保用到的是各自的實作
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
static inline void gPixel(int x, int y, uint16_t c) { GO(drawPixel(x - ox, y - oy, c)); }

// 565 快速混色：a = 0~32（0 全是 bg，32 全是 fg）
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
  uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
  uint32_t r = ((((f - b) * a) >> 5) + b) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

// 透明背景文字（只畫字，不填底色）：疊在背景大數字上用
static void gText(const String &s, int x, int y, int font, uint16_t fg, uint8_t datum = TL_DATUM) {
  GO(setTextDatum(datum));
  GO(setTextColor(fg));
  GO(drawString(s, x - ox, y - oy, font));
}

// 半透明圓角卡片：把底下已經畫好的內容（含背景大數字）混入淡紫色。只處理目前這條橫帶範圍內的列
static void gCard(int x, int y, int w, int h, int r) {
  if (!useSpr) { gRRect(x, y, w, h, r, C_CARDSOLID); return; }   // 備援：實心卡片
  int ys = y > curStripY ? y : curStripY;
  int ye = (y + h < curStripY + STRIP_H) ? y + h : curStripY + STRIP_H;
  for (int py = ys; py < ye; py++) {
    int dy = py - y; if (y + h - 1 - py < dy) dy = y + h - 1 - py;   // 離上緣或下緣幾列
    int inset = 0;
    if (dy < r) { float t = r - dy - 0.5f; inset = r - (int)(sqrtf((float)(r * r) - t * t) + 0.5f); }
    for (int px = x + inset; px < x + w - inset; px++) {
      uint16_t b = spr.readPixel(px - ox, py - oy);
      spr.drawPixel(px - ox, py - oy, blend565(C_CARD, b, CARD_A5));
    }
  }
}

// 14x14 小圖示（左上角 x,y）
static void iconClock(int x, int y, uint16_t c) {
  gCircleO(x + 7, y + 7, 6, c); gCircleO(x + 7, y + 7, 5, c);
  gLine(x + 7, y + 7, x + 7, y + 4, c); gLine(x + 7, y + 7, x + 10, y + 9, c);
}
static void iconNozzle(int x, int y, uint16_t c) {
  gRect(x + 3, y + 1, 8, 5, c); gTri(x + 3, y + 6, x + 11, y + 6, x + 7, y + 10, c); gRect(x + 6, y + 10, 2, 4, c);
}
static void iconBed(int x, int y, uint16_t c) {
  gRRect(x, y + 9, 14, 5, 2, c);
  for (int i = 0; i < 3; i++) { int xx = x + 2 + i * 4; gLine(xx, y + 1, xx + 1, y + 3, c); gLine(xx + 1, y + 3, xx, y + 5, c); gLine(xx, y + 5, xx + 1, y + 7, c); }
}

static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {  // t=0 → a，t=255 → b
  int ar = ((a >> 11) & 0x1F) << 3, ag = ((a >> 5) & 0x3F) << 2, ab = (a & 0x1F) << 3;
  int br = ((b >> 11) & 0x1F) << 3, bg = ((b >> 5) & 0x3F) << 2, bb = (b & 0x1F) << 3;
  return RGB565(ar + (br - ar) * t / 255, ag + (bg - ag) * t / 255, ab + (bb - ab) * t / 255);
}

// ---- 背景大數字：兩位數固定（00~99）、七段顯示器造型、實心色塊，沒有外暈 ----

static const uint8_t SEG7[10] = {  // bit0..6 = a,b,c,d,e,f,g
  0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
  0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};

static int bgNum = -1;  // 0~99；-1 = 不顯示（完成 / 閒置時）

// 橫向六角形線段：兩端是尖的，中心線在 yc，粗 t
static void segH(int x0, int x1, int yc, int t, uint16_t col) {
  gRect(x0 + t / 2 - 1, yc - t / 2, (x1 - x0) - t + 2, t, col);
  gTri(x0, yc, x0 + t / 2, yc - t / 2, x0 + t / 2, yc + t / 2, col);
  gTri(x1, yc, x1 - t / 2, yc - t / 2, x1 - t / 2, yc + t / 2, col);
}

// 直向六角形線段：兩端是尖的，中心線在 xc，粗 t
static void segV(int xc, int y0, int y1, int t, uint16_t col) {
  gRect(xc - t / 2, y0 + t / 2 - 1, t, (y1 - y0) - t + 2, col);
  gTri(xc, y0, xc - t / 2, y0 + t / 2, xc + t / 2, y0 + t / 2, col);
  gTri(xc, y1, xc - t / 2, y1 - t / 2, xc + t / 2, y1 - t / 2, col);
}

static void drawSeg7Digit(int d, int x, int y, int w, int h, int t, uint16_t col) {
  const int e = 3;                         // 線段之間的縫
  int mid = y + h / 2;
  int xl = x + t / 2, xr = x + w - t / 2;  // 左右兩條直線段的中心線
  int yt = y + t / 2, yb = y + h - t / 2;  // 上下兩條橫線段的中心線
  uint8_t m = SEG7[d];
  if (m & 0x01) segH(xl + e, xr - e, yt, t, col);        // a 上
  if (m & 0x02) segV(xr, yt + e, mid - e, t, col);       // b 右上
  if (m & 0x04) segV(xr, mid + e, yb - e, t, col);       // c 右下
  if (m & 0x08) segH(xl + e, xr - e, yb, t, col);        // d 下
  if (m & 0x10) segV(xl, mid + e, yb - e, t, col);       // e 左下
  if (m & 0x20) segV(xl, yt + e, mid - e, t, col);       // f 左上
  if (m & 0x40) segH(xl + e, xr - e, mid, t, col);       // g 中
}

static void drawBgNumber() {
  if (bgNum < 0) return;
  uint16_t col = mix565(C_BG, C_WHITE, bgLevel);
  // 條帶寬 184（x 28~212）：兩個數字各 88 寬、間隔 8，剛好塞滿；高 196（y 22~218）
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

static inline bool vis(int y0, int y1) { return y1 > curStripY && y0 < curStripY + STRIP_H; }

// 縮圖（84x84，從暫存檔逐列讀出，透明像素 0x0001 跳過）
static void drawThumbImage() {
  const int X = 30, Y = 72;
  if (!tf) tf = LittleFS.open(THUMB_FILE, "r");
  if (!tf) return;
  int y0 = Y > curStripY ? Y : curStripY;
  int y1 = (Y + THUMB_H < curStripY + STRIP_H) ? Y + THUMB_H : curStripY + STRIP_H;
  uint8_t line[THUMB_W * 2];
  for (int y = y0; y < y1; y++) {
    if (!tf.seek((uint32_t)(y - Y) * THUMB_W * 2, SeekSet)) return;
    if (tf.read(line, sizeof(line)) != sizeof(line)) return;
    for (int x = 0; x < THUMB_W; x++) {
      uint16_t v = line[x * 2] | (line[x * 2 + 1] << 8);
      if (v != TD_TRANSP) gPixel(X + x, y, v);
    }
  }
}

// 右側的小卡片：圖示 + 數值（右對齊）；數值太寬就自動改小字
static void miniCard(int y, int kind, uint16_t col, const String &val, bool deg) {
  const int x = 122, w = 90, h = 33;
  if (!vis(y, y + h)) return;
  gCard(x, y, w, h, 9);
  if (kind == 0) iconClock(x + 8, y + 9, col);
  else if (kind == 1) iconNozzle(x + 8, y + 9, col);
  else iconBed(x + 8, y + 9, col);
  int right = x + w - 8 - (deg ? 7 : 0);
  int avail = right - (x + 8 + 14 + 4);
  int font = tft.textWidth(val, 4) <= avail ? 4 : 2;
  gText(val, right, y + (font == 4 ? 3 : 8), font, C_WHITE, TR_DATUM);
  if (deg) gCircleO(right + 4, y + 9, 2, C_WHITE);
}

// 把所有前景元素畫到目前的繪圖目標；不在這一條橫帶範圍內的元素直接跳過
static void drawForeground() {
  if (vis(22, 40)) {  // 狀態列：圓點 + 狀態（底線）+ 右上角小字顯示小電視自己的 IP
    gCircle(34, 31, 5, cur.stateColor);
    gText(cur.state, 44, 23, 2, C_WHITE);
    gLine(44, 38, 44 + tft.textWidth(cur.state, 2), 38, C_WHITE);
    gText(WiFi.localIP().toString(), 212, 27, 1, C_GREY, TR_DATUM);
  }
  if (vis(40, 56)) {  // 檔名（去掉 .gcode，太長就截斷加 ..）
    String fn = cur.filename;
    if (fn.endsWith(".gcode")) fn.remove(fn.length() - 6);
    bool cut = false;
    while (fn.length() > 1 && tft.textWidth(fn + (cut ? ".." : ""), 2) > 184) {
      fn.remove(fn.length() - 1);
      cut = true;
    }
    gText(fn + (cut ? ".." : ""), 28, 41, 2, C_GREY);
  }
  if (vis(60, 168)) {  // 左：縮圖卡片
    gCard(28, 60, 88, 108, 9);
    bool showThumb = thumbOk && cur.state != "IDLE" && cur.state != "OFFLINE";
    if (showThumb) drawThumbImage();
    else gText(cur.state == "IDLE" ? "NO JOB" : "NO IMG", 72, 108, 2, C_GREY, TC_DATUM);
  }
  // 右：剩餘時間、噴頭、熱床
  miniCard(60, 0, C_GREY, fmtEta(cur.etaS), false);
  miniCard(97, 1, C_ORANGE, cur.nozzle < 0 ? String("--") : String(cur.nozzle), cur.nozzle >= 0);
  miniCard(134, 2, C_BLUE, cur.bed < 0 ? String("--") : String(cur.bed), cur.bed >= 0);

  if (vis(172, 222)) {  // 下：四個卡槽 + 材質，使用中的用白色粗框；卡片盡量貼近底部
    gCard(28, 172, 184, 47, 9);   // 卡片 y 172~218，底下留 3 列（219~221）的縫給外圈
    for (int i = 0; i < 4; i++) {
      int x = 32 + (i * 138) / 3;
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
        gText(String(cur.slots[i].mat), x + 19, y + 24, 2, act ? C_WHITE : C_GREY, TC_DATUM);
      } else {
        gRRect(x, y, 38, 22, 5, C_EMPTY);
        gRRectO(x, y, 38, 22, 5, C_BORDER);
        gLine(x + 13, y + 18, x + 25, y + 4, C_GREY);
        gText("--", x + 19, y + 24, 2, C_GREY, TC_DATUM);
      }
    }
  }
}

// 組合並推送一條橫帶（y0 起算 25 列）：背景 → 背景大數字 → 前景，全部在離屏畫好後一次推上螢幕，
// 螢幕上的每個像素都是「舊畫面 → 新畫面」直接替換，中間不會出現被清空的狀態，所以不閃
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
  } else {  // 記憶體不夠建 sprite 時的備援：直接畫在螢幕上，用 viewport 限制只畫這一條
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
  if (pct > 99) pct = 99;  // 固定兩位數，100% 不處理
  bool showNum = (cur.state == "PRINTING" || cur.state == "PAUSED");  // 完成 / 閒置 / 離線不顯示數字
  int num = showNum ? pct : -1;
  static int shownNum = -2;
  static int shownThumbVer = -1;

  // 外圈進度條（直接畫在螢幕上，只補畫新增的那一小段）
  float len = constrain(cur.progress, 0.0f, 1.0f) * RING_TOTAL;
  if (force || cur.stateColor != shown.stateColor || len < shownLen - 1) {
    drawRing(0, RING_TOTAL, C_TRACK);
    drawRing(0, len, cur.stateColor);
    shownLen = len;
  } else if (len > shownLen + 1.5f) {
    drawRing(shownLen, len, cur.stateColor);
    shownLen = len;
  }

  // 找出哪幾條橫帶需要重畫
  uint8_t dirty = 0;
  auto mark = [&](int y0, int y1) {
    for (int k = 0; k < STRIP_N; k++) {
      int s0 = STRIP_Y0 + k * STRIP_H, s1 = s0 + STRIP_H - 1;
      if (y1 >= s0 && y0 <= s1) dirty |= (1 << k);
    }
  };
  if (force || num != shownNum) {
    dirty = 0xFF;  // 背景數字變了：整個中央區域重畫（逐條推送，不閃）
  } else {
    if (cur.state != shown.state) mark(22, 40);
    if (cur.filename != shown.filename) mark(40, 56);
    if (cur.etaS != shown.etaS) mark(60, 93);
    if (cur.nozzle != shown.nozzle) mark(97, 130);
    if (cur.bed != shown.bed) mark(134, 167);
    if (!slotsEqual(cur, shown)) mark(172, 221);
    if (thumbVer != shownThumbVer) mark(60, 168);
  }
  bgNum = num;
  shownNum = num;
  shownThumbVer = thumbVer;
  for (int k = 0; k < STRIP_N; k++)
    if (dirty & (1 << k)) { composeStrip(STRIP_Y0 + k * STRIP_H); yield(); }

  shown = cur;
  shownValid = true;
}

// ====================== 外圈流水特效 ======================
// 列印中，一條「頭亮尾淡」的亮帶沿著已完成的進度條順時針流動
#if FLOW_EFFECT
static float flowHead = 0;
static unsigned long nextFlow = 0;

static uint16_t tint(uint16_t c, uint8_t a) {  // 往白色混合，a=0 原色 → 255 全白
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

  float len = shownLen;                 // 目前已畫出的進度長度
  if (len < 14) return;
  float L = FLOW_LEN < len ? FLOW_LEN : len;
  float oldHead = flowHead;
  float newHead = oldHead + FLOW_STEP;

  // 1) 把剛離開的尾巴還原成進度條底色
  float eFrom = oldHead - L; if (eFrom < 0) eFrom = 0;
  float eTo = newHead - L;   if (eTo > len) eTo = len;
  if (eTo > eFrom) drawRing(eFrom, eTo, cur.stateColor);

  // 2) 整條亮帶已經滑出進度條尾端 → 回到起點重來
  if (newHead - L > len) { flowHead = 0; return; }

  // 3) 畫新的亮帶：尾巴淡、頭部亮
  float a0 = newHead - L; if (a0 < 0) a0 = 0;
  float a1 = newHead;     if (a1 > len) a1 = len;
  int x, y;
  for (float sp = a0; sp <= a1; sp += 2.0f) {
    float t = (sp - (newHead - L)) / L;   // 0 尾 → 1 頭
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
  const char *p = s + n - 6;  // 取最後 6 碼（"01b04ae" → "1b04ae"）
  for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)p[i])) return false;
  out = strtoul(p, nullptr, 16);
  return true;
}

// ---- 抓縮圖：只下載 G-code 檔案開頭一小段，找出 96x96 的 PNG 縮圖，解碼後縮成 84x84 存檔 ----
struct FsStore {
  File win, out;
  bool open() {
    win = LittleFS.open("/win.tmp", "w+");
    out = LittleFS.open("/thumb.new", "w");
    return win && out;
  }
  bool winWrite(uint32_t pos, const uint8_t *d, size_t n) { return win.seek(pos, SeekSet) && win.write(d, n) == n; }
  bool winRead(uint32_t pos, uint8_t *d, size_t n) { return win.seek(pos, SeekSet) && win.read(d, n) == n; }
  bool outWrite(uint32_t pos, const uint8_t *d, size_t n) { return out.seek(pos, SeekSet) && out.write(d, n) == n; }
  void tick() { yield(); }
  void close() { win.close(); out.close(); }
};

static String urlEncode(const String &s) {
  String o;
  char b[4];
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isAlphaNumeric((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += c;
    else { snprintf(b, sizeof(b), "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

static bool b64val(char c, int &v) {
  if (c >= 'A' && c <= 'Z') v = c - 'A';
  else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
  else if (c >= '0' && c <= '9') v = c - '0' + 52;
  else if (c == '+') v = 62;
  else if (c == '/') v = 63;
  else return false;
  return true;
}

// 從串流讀一行；沒資料或連線關閉就回 -1
static int readLine(WiFiClient &c, char *buf, int cap, unsigned long timeoutMs) {
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (c.available()) {
      int ch = c.read();
      if (ch < 0) continue;
      t0 = millis();
      if (ch == '\n') { buf[n] = 0; return n; }
      if (ch != '\r' && n < cap - 1) buf[n++] = (char)ch;
    } else {
      if (!c.connected()) break;
      delay(1);
    }
  }
  buf[n] = 0;
  return n > 0 ? n : -1;
}

static void fetchThumb(const String &fn) {
  thumbName = fn;
  thumbOk = false;
  closeThumbFile();
  thumbVer++;
  if (printerHost.length() == 0) return;
  if (ESP.getMaxFreeBlockSize() < 11000) { Serial.println("thumb: not enough RAM, skipped"); return; }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  http.addHeader("Range", "bytes=0-20479");   // 縮圖在檔案最前面，只要開頭 20KB
  if (!http.begin(client, "http://" + printerHost + "/server/files/gcodes/" + urlEncode(fn))) return;
  int code = http.GET();
  if (code != 200 && code != 206) { http.end(); Serial.printf("thumb: HTTP %d\n", code); return; }
  WiFiClient &c = http.getStream();

  char line[128];
  uint8_t *png = nullptr;
  int pngLen = 0, pngCap = 0, qn = 0, q[4];
  bool inBlock = false;
  for (int guard = 0; guard < 500; guard++) {
    int n = readLine(c, line, sizeof(line), 3000);
    if (n < 0) break;
    if (!inBlock) {
      int w, h, len;
      if (sscanf(line, "; thumbnail begin %dx%d %d", &w, &h, &len) == 3 && w <= TD_MAXW && h <= TD_MAXW && len > 0 && len <= 9000) {
        pngCap = len / 4 * 3 + 8;
        png = (uint8_t *)malloc(pngCap);
        if (!png) break;
        inBlock = true;
        qn = 0;
      }
    } else {
      if (strncmp(line, "; thumbnail end", 15) == 0) break;
      for (char *p = line; *p; p++) {
        int v;
        if (!b64val(*p, v)) continue;            // 略過 ';'、空白、'='
        q[qn++] = v;
        if (qn == 4 && pngLen + 3 <= pngCap) {
          png[pngLen++] = (q[0] << 2) | (q[1] >> 4);
          png[pngLen++] = ((q[1] & 15) << 4) | (q[2] >> 2);
          png[pngLen++] = ((q[2] & 3) << 6) | q[3];
          qn = 0;
        }
      }
    }
  }
  http.end();
  if (png && qn >= 2 && pngLen + 2 <= pngCap) {   // 結尾不足 4 個字元的部分
    png[pngLen++] = (q[0] << 2) | (q[1] >> 4);
    if (qn == 3) png[pngLen++] = ((q[1] & 15) << 4) | (q[2] >> 2);
  }
  if (!png || pngLen < 40) { free(png); Serial.println("thumb: no thumbnail in file"); return; }

  bool ok = false;
  FsStore st;
  if (st.open()) {
    ThumbDecoder<FsStore> *dec = new ThumbDecoder<FsStore>(st);   // 5.7KB，放 heap 不放 stack
    if (dec) { ok = dec->decode(png, pngLen, THUMB_W, THUMB_H); delete dec; }
    st.close();
  }
  free(png);
  LittleFS.remove("/win.tmp");
  if (ok) { LittleFS.remove(THUMB_FILE); ok = LittleFS.rename("/thumb.new", THUMB_FILE); }
  else LittleFS.remove("/thumb.new");
  thumbOk = ok;
  thumbVer++;
  Serial.printf("thumb: %s (%d bytes png), free heap %u\n", ok ? "ok" : "decode failed", pngLen, ESP.getFreeHeap());
}

// ---- 剩餘時間估算 ----
// 只用「檔案位置進度」往後推算會嚴重失準：開頭的慢速第一層佔時間多、佔檔案位元組少，
// 剛開始印時進度只有 1% 但已經過了十幾分鐘，推出來的剩餘時間會是真實值的好幾倍。
// 所以優先使用切片軟體估的總時間（Creality Print 會把它寫在檔名最後，例如 xxx_PLA_3h12m.gcode），
// 再隨著進度增加，逐步改用實際速度推算。

// 從檔名最後一段解析預估總時間（支援 d、h、m、s），解析不出來回傳 0
static int estFromFilename(const String &fn) {
  int dot = fn.lastIndexOf('.');
  String base = dot > 0 ? fn.substring(0, dot) : fn;
  int us = base.lastIndexOf('_');
  String seg = us >= 0 ? base.substring(us + 1) : base;
  long total = 0, num = 0;
  bool have = false;
  for (unsigned i = 0; i < seg.length(); i++) {
    char c = seg[i];
    if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); have = true; }
    else if (have && (c == 'd' || c == 'h' || c == 'm' || c == 's')) {
      total += num * (c == 'd' ? 86400L : c == 'h' ? 3600L : c == 'm' ? 60L : 1L);
      num = 0;
      have = false;
    } else return 0;
  }
  if (have || total < 60 || total > 30L * 86400L) return 0;
  return (int)total;
}

// dur: 已列印秒數；prog: 檔案進度 0~1；estT: 檔名預估總秒數（0 = 沒有）；回傳剩餘秒數，-1 = 算不出來
static int computeEta(float dur, float prog, int estT) {
  float e1 = -1, e2 = -1;
  if (estT > 0) e1 = estT - dur;                                  // 依預估總時間
  if (prog >= 0.05f) e2 = dur * (1.0f - prog) / prog;             // 依實際速度（進度夠大才可信）
  if (estT > 0 && e1 <= 0) return e2 >= 0 ? (int)e2 : -1;         // 已超過預估時間：改用實際速度，算不出來就顯示 --
  if (e1 >= 0 && e2 >= 0) {                                       // 兩者都有：進度越大越相信實際速度
    float w = prog > 1.0f ? 1.0f : prog;
    return (int)((1.0f - w) * e1 + w * e2);
  }
  if (e1 >= 0) return (int)e1;
  if (e2 >= 0) return (int)e2;
  return -1;
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
  http.useHTTP10(true);  // 避免 chunked，讓 ArduinoJson 直接讀串流
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
  d.nozzle = (int)((float)(st["extruder"]["temperature"] | -1.0f) + 0.5f);
  d.bed = (int)((float)(st["heater_bed"]["temperature"] | -1.0f) + 0.5f);
  if (d.nozzle < 0) d.nozzle = -1;
  if (d.bed < 0) d.bed = -1;

  bool running = (s == "printing" || s == "paused");
  if (running && d.filename.length() > 0 && d.filename != thumbName) fetchThumb(d.filename);   // 新檔案才抓縮圖
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
    d.etaS = computeEta(dur, prog, estFromFilename(d.filename));
  } else {
    d.elapsedS = -1;
    d.etaS = -1;
  }
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
input[type=range]{padding:0;border:0;background:none;height:32px;accent-color:#3ddc84}
b{color:#fff}
button{padding:10px 16px;font-size:15px;border:0;border-radius:8px;background:#3ddc84;color:#000;margin:10px 8px 0 0}
button.g{background:#333;color:#eee}button.r{background:#a33;color:#fff}
button.rb{background:#333;color:#eee;margin:6px 8px 0 0}button.rb.on{background:#3ddc84;color:#000}
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
function s(save){document.getElementById(out).textContent=e.value;clearTimeout(T[id]);
T[id]=setTimeout(function(){fetch('/api/set?'+k+'='+e.value+(save?'&save=1':''))},save?0:100)}
e.addEventListener('input',function(){s(0)});e.addEventListener('change',function(){s(1)})}
bind('br','bright','bv');bind('bg','bg','gv');bind('pl','poll','pv');
document.querySelectorAll('#rot button').forEach(function(b){b.addEventListener('click',function(){
document.querySelectorAll('#rot button').forEach(function(x){x.classList.remove('on')});b.classList.add('on');
fetch('/api/set?rot='+b.dataset.r+'&save=1')})});
</script></body></html>)HTML";

static String row(const String &k, const String &v) {
  return "<tr><td>" + k + "</td><td>" + htmlEscape(v) + "</td></tr>";
}

void handleRoot() {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");
  web.sendContent_P(PAGE_HEAD);
  // 注意：chunked 模式下 sendContent("") 會直接結束回應，所以空字串不能送
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
                   "<input type=\"range\" id=\"pl\" min=\"1\" max=\"30\" value=\"" + pollSec + "\"></div>";
  web.sendContent(sliders);
  String rot = "<div class=\"c\"><label>畫面旋轉</label><div id=\"rot\">";
  for (int i = 0; i < 4; i++)
    rot += String("<button type=\"button\" class=\"rb") + (i == rotation ? " on" : "") + "\" data-r=\"" + i + "\">" + (i * 90) + "°</button>";
  rot += "</div></div>";
  web.sendContent(rot);
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

// 旋轉畫面：設定 TFT 的方向（240x240 的位址偏移由 TFT_eSPI 自動處理），再整個畫面重畫
void applyRotation(int r) {
  rotation = constrain(r, 0, 3);
  tft.setRotation(rotation);
  tft.fillScreen(C_BG);
  Screen s = screen;
  screen = SCR_NONE;  // 強迫 setScreen 重畫目前的畫面（配網 / 設定提示 / 主畫面）
  setScreen(s);
  if (s == SCR_DASH) {
    shownValid = false;
    renderDash();
  }
}

// 滑塊即時套用（不寫入）；放開滑塊時網頁會帶 save=1 才寫入快閃記憶體
void handleSet() {
  if (web.hasArg("bright")) setBacklight(web.arg("bright").toInt());
  if (web.hasArg("bg")) {
    bgLevel = constrain((int)web.arg("bg").toInt(), 0, 200);
    if (screen == SCR_DASH) {
      shownValid = false;  // 整個畫面重畫一次，立刻看到新的亮度
      renderDash();
    }
  }
  if (web.hasArg("poll")) {
    pollSec = constrain((int)web.arg("poll").toInt(), 1, 30);
    nextPoll = millis() + (unsigned long)pollSec * 1000UL;
  }
  if (web.hasArg("rot")) {
    int r = constrain((int)web.arg("rot").toInt(), 0, 3);
    if (r != rotation) applyRotation(r);
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
  analogWriteRange(BL_PWM_RANGE);  // ESP8266 核心 3.x 預設範圍是 255，這裡明確設成 1023

  LittleFS.begin();
  loadConfig();
  setBacklight(brightPct);

  tft.init();
  tft.setRotation(rotation);
  sprOk = spr.createSprite(STRIP_W, STRIP_H) != nullptr;   // 最早配置，避免之後記憶體碎片化配不到
  Serial.printf("sprite %s, free heap %u\n", sprOk ? "ok" : "FAILED", ESP.getFreeHeap());
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

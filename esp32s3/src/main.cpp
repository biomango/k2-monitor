#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <XPowersLib.h>
#include <JPEGDEC.h>
#include "esp_heap_caps.h"
#include "TouchDrvCSTXXX.hpp"
#include "cjk_font.h"

static constexpr int LCD_SDIO0 = 4;
static constexpr int LCD_SDIO1 = 5;
static constexpr int LCD_SDIO2 = 6;
static constexpr int LCD_SDIO3 = 7;
static constexpr int LCD_SCLK = 38;
static constexpr int LCD_RESET = 39;
static constexpr int LCD_CS = 12;
static constexpr int LCD_WIDTH = 466;
static constexpr int LCD_HEIGHT = 466;
static constexpr int IIC_SDA = 15;
static constexpr int IIC_SCL = 14;
static constexpr int TP_INT = 11;
static constexpr int TP_RST = 40;
static constexpr uint8_t TOUCH_ADDR = 0x5A;

static constexpr uint16_t BG = 0x0000;
static constexpr uint16_t PANEL = 0x18E3;
static constexpr uint16_t WHITE = 0xFFFF;
static constexpr uint16_t MUTED = 0x9CF3;
static constexpr uint16_t GREEN = 0x07E0;
static constexpr uint16_t AMBER = 0xFD20;
static constexpr uint16_t RED = 0xF800;
static constexpr uint16_t BLUE = 0x3DFF;

static XPowersPMU panelPmu;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    displayBus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

TouchDrvCST92xx touch;
int16_t touchX[2] = {};
int16_t touchY[2] = {};
WebServer web(80);
Preferences prefs;

enum class Page : uint8_t { Dashboard, Camera, Settings };
Page page = Page::Dashboard;

struct SlotData { bool has = false; uint32_t rgb = 0; String material; };

struct PrinterData {
  String state = "連線中";
  String filename = "";
  float progress = 0;
  int nozzle = -1;
  int bed = -1;
  int elapsed = -1;
  int eta = -1;
  SlotData slots[4];
  int activeSlot = -1;
  bool online = false;
};
PrinterData printer;

String printerHost;
String cameraUrl;
WiFiClient cameraClient;
JPEGDEC jpeg;
uint8_t *cameraFrame = nullptr;
size_t cameraFrameCapacity = 0;
int cameraContentLength = 0;
int cameraFramePos = 0;
int cameraFrameState = 0;
uint32_t lastCameraDataMs = 0;
uint32_t nextPoll = 0;
uint32_t lastTouch = 0;
uint32_t lastCameraAttempt = 0;
String cameraMessage = "攝影機串流尚未設定";

static String hostForMoonraker() {
  if (printerHost.indexOf(':') >= 0) return printerHost;
  return printerHost + ":7125";
}

static int cjkGlyphIndex(uint32_t codepoint) {
  int low = 0;
  int high = sizeof(CJK_CODEPOINTS) / sizeof(CJK_CODEPOINTS[0]) - 1;
  while (low <= high) {
    const int mid = (low + high) / 2;
    const uint16_t value = pgm_read_word(&CJK_CODEPOINTS[mid]);
    if (value == codepoint) return mid;
    if (value < codepoint) low = mid + 1;
    else high = mid - 1;
  }
  return -1;
}

static int drawUtf8At(const String &s, int x, int y, uint16_t color, uint8_t scale) {
  gfx->setTextSize(scale);
  gfx->setTextColor(color);
  for (size_t i = 0; i < s.length();) {
    const uint8_t b0 = (uint8_t)s[i];
    uint32_t cp = b0;
    size_t count = 1;
    if ((b0 & 0xE0) == 0xC0 && i + 1 < s.length()) {
      cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F);
      count = 2;
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.length()) {
      cp = ((uint32_t)(b0 & 0x0F) << 12) |
           (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F);
      count = 3;
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.length()) {
      cp = ((uint32_t)(b0 & 0x07) << 18) |
           (((uint8_t)s[i + 1] & 0x3F) << 12) |
           (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F);
      count = 4;
    }
    i += count;

    if (cp < 0x80) {
      gfx->setCursor(x, y);
      gfx->write((uint8_t)cp);
      x += 6 * scale;
      continue;
    }

    const int glyph = cjkGlyphIndex(cp);
    if (glyph < 0) {
      gfx->setCursor(x, y);
      gfx->write('?');
      x += 6 * scale;
      continue;
    }
    const size_t offset = (size_t)glyph * 8;
    for (int row = 0; row < 8; ++row) {
      const uint8_t bits = pgm_read_byte(&CJK_BITMAPS[offset + row]);
      for (int col = 0; col < 8; ++col) {
        if (bits & (0x80 >> col)) {
          gfx->fillRect(x + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
    x += 8 * scale;
  }
  return x;
}

static void drawCentered(const String &s, int y, uint8_t size, uint16_t color) {
  const uint8_t scale = size >= 4 ? 4 : (size >= 3 ? 3 : 2);
  int width = 0;
  for (size_t i = 0; i < s.length();) {
    const uint8_t b = (uint8_t)s[i];
    if (b < 0x80) { width += 6 * scale; ++i; }
    else if ((b & 0xE0) == 0xC0) { width += 8 * scale; i += 2; }
    else if ((b & 0xF0) == 0xE0) { width += 8 * scale; i += 3; }
    else { width += 8 * scale; i += 4; }
  }
  drawUtf8At(s, (LCD_WIDTH - width) / 2, y, color, scale);
}

static void drawHeader(const char *title) {
  gfx->fillScreen(BG);
  gfx->fillRoundRect(24, 22, 418, 58, 22, PANEL);
  drawCentered(title, 40, 2, WHITE);
}

static void drawNav() {
  const int y = 414;
  gfx->fillRoundRect(24, y, 418, 42, 18, PANEL);
  drawUtf8At("總覽", 58, y + 12, page == Page::Dashboard ? BLUE : MUTED, 2);
  drawUtf8At("相機", 190, y + 12, page == Page::Camera ? BLUE : MUTED, 2);
  drawUtf8At("設定", 328, y + 12, page == Page::Settings ? BLUE : MUTED, 2);
}

static uint16_t stateColor() {
  if (printer.state == "列印中") return GREEN;
  if (printer.state == "暫停") return AMBER;
  if (printer.state == "錯誤") return RED;
  if (!printer.online) return RED;
  return MUTED;
}

static String formatDuration(int seconds);

static void drawDashboard() {
  page = Page::Dashboard;
  drawHeader("K2 PLUS");
  drawCentered(printer.state, 104, 3, stateColor());
  gfx->drawCircle(233, 235, 104, PANEL);
  gfx->drawCircle(233, 235, 92, PANEL);
  gfx->drawArc(233, 235, 96, 88, 135, 405, BLUE);
  gfx->setTextSize(5);
  gfx->setTextColor(WHITE);
  String pct = String((int)(printer.progress * 100 + 0.5f)) + "%";
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(pct.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((LCD_WIDTH - (int)w) / 2, 210);
  gfx->print(pct);
  drawCentered(printer.filename.length() ? printer.filename : "尚無列印工作", 298, 1, MUTED);
  drawCentered("剩餘 " + formatDuration(printer.eta) + "  已列印 " + formatDuration(printer.elapsed), 318, 1, MUTED);
  for (int i = 0; i < 4; ++i) {
    const int x = 88 + i * 96;
    const uint16_t color = printer.slots[i].has
      ? gfx->color565((printer.slots[i].rgb >> 16) & 0xFF, (printer.slots[i].rgb >> 8) & 0xFF, printer.slots[i].rgb & 0xFF)
      : PANEL;
    gfx->fillCircle(x, 348, 14, color);
    gfx->drawCircle(x, 348, 16, printer.activeSlot == i ? WHITE : MUTED);
    gfx->setTextSize(1); gfx->setTextColor(MUTED);
    gfx->setCursor(x - 8, 369); gfx->print("1"); gfx->print((char)('A' + i));
  }
  gfx->fillRoundRect(34, 382, 190, 30, 10, PANEL);
  gfx->fillRoundRect(242, 382, 190, 30, 10, PANEL);
  drawUtf8At("噴頭 ", 48, 390, WHITE, 2);
  drawUtf8At(printer.nozzle < 0 ? "--" : String(printer.nozzle) + "°", 110, 390, WHITE, 2);
  drawUtf8At("熱床 ", 256, 390, WHITE, 2);
  drawUtf8At(printer.bed < 0 ? "--" : String(printer.bed) + "°", 318, 390, WHITE, 2);
  drawNav();
}


static int drawCameraJpeg(JPEGDRAW *draw) {
  const int targetW = 410;
  const int targetH = 230;
  const int left = 28 + (targetW - draw->iWidth) / 2;
  const int top = 104 + (targetH - draw->iHeight) / 2;
  gfx->draw16bitRGBBitmap(left + draw->x, top + draw->y,
                          draw->pPixels, draw->iWidth, draw->iHeight);
  return 1;
}

static void stopCameraStream() {
  if (cameraClient.connected()) cameraClient.stop();
  cameraFrameState = 0;
  cameraContentLength = 0;
}

static bool startCameraStream() {
  if (printerHost.isEmpty()) return false;
  String ip = printerHost;
  const int colon = ip.indexOf(':');
  if (colon >= 0) ip = ip.substring(0, colon);
  if (!cameraClient.connect(ip.c_str(), 4408)) {
    cameraMessage = "無法連線：請安裝 Camera Support";
    return false;
  }
  cameraClient.setTimeout(30);
  cameraClient.print("GET /go2rtc/api/stream.mjpeg?src=k2plus_mjpeg HTTP/1.1\r\n");
  cameraClient.print("Host: " + ip + ":4408\r\n");
  cameraClient.print("Accept: multipart/x-mixed-replace\r\n");
  cameraClient.print("Connection: keep-alive\r\n\r\n");
  cameraFrameState = 0;
  cameraContentLength = 0;
  lastCameraDataMs = millis();
  cameraMessage = "正在連接 K2 Plus 相機…";
  return true;
}

static void renderCameraFrame() {
  if (!cameraFrame || cameraContentLength <= 0) return;
  if (jpeg.openRAM(cameraFrame, cameraContentLength, drawCameraJpeg)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    gfx->fillRect(29, 105, 408, 228, BG);
    jpeg.decode(0, 0, JPEG_SCALE_QUARTER);
    jpeg.close();
    cameraMessage = "即時串流";
  } else {
    cameraMessage = "JPEG 解碼失敗";
  }
}

static void cameraTick() {
  if (page != Page::Camera || WiFi.status() != WL_CONNECTED) return;
  if (!cameraClient.connected()) {
    if (millis() - lastCameraAttempt > 3000) {
      lastCameraAttempt = millis();
      startCameraStream();
    }
    return;
  }
  if (millis() - lastCameraDataMs > 8000) {
    stopCameraStream();
    cameraMessage = "串流逾時，重新連線中…";
    return;
  }
  if (!cameraClient.available()) return;

  String line = cameraClient.readStringUntil('\n');
  line.trim();
  lastCameraDataMs = millis();

  if (cameraFrameState == 0) {
    if (line.startsWith("HTTP/") && !line.endsWith("200 OK")) {
      stopCameraStream();
      cameraMessage = "相機服務未啟用（HTTP 非 200）";
      return;
    }
    if (line.isEmpty()) cameraFrameState = 1;
    return;
  }

  if (cameraFrameState == 1) {
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
      cameraContentLength = line.substring(line.indexOf(':') + 1).toInt();
      return;
    }
    if (line.isEmpty() && cameraContentLength > 0) {
      if ((size_t)cameraContentLength > cameraFrameCapacity) {
        uint8_t *next = (uint8_t *)heap_caps_realloc(cameraFrame, cameraContentLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) {
          stopCameraStream();
          cameraMessage = "記憶體不足，無法載入影像";
          return;
        }
        cameraFrame = next;
        cameraFrameCapacity = cameraContentLength;
      }
      cameraFramePos = 0;
      cameraFrameState = 2;
    }
    return;
  }

  if (cameraFrameState == 2) {
    const int available = cameraClient.available();
    const int remaining = cameraContentLength - cameraFramePos;
    if (available > 0 && remaining > 0) {
      const int chunk = available < remaining ? available : remaining;
      const int got = cameraClient.read(cameraFrame + cameraFramePos, chunk);
      if (got > 0) cameraFramePos += got;
    }
    if (cameraFramePos >= cameraContentLength) {
      renderCameraFrame();
      cameraContentLength = 0;
      cameraFramePos = 0;
      cameraFrameState = 1;
    }
  }
}

static void drawCameraPage() {
  page = Page::Camera;
  drawHeader("印表機相機");
  gfx->drawRoundRect(28, 104, 410, 230, 18, MUTED);
  drawCentered("即時影像", 130, 2, WHITE);
  drawCentered(cameraMessage, 190, 1, AMBER);
  drawCentered("需啟用 K2 Plus Camera Support", 235, 1, MUTED);
  drawCentered("MJPEG / go2rtc 串流", 270, 1, MUTED);
  drawNav();
}

static void drawSettingsPage() {
  page = Page::Settings;
  drawHeader("網路設定");
  drawCentered("印表機：" + (printerHost.length() ? printerHost : "尚未設定"), 150, 1, WHITE);
  drawCentered("裝置 IP：" + WiFi.localIP().toString(), 205, 1, MUTED);
  drawCentered("設定頁面：http://" + WiFi.localIP().toString(), 255, 1, BLUE);
  drawNav();
}

static void renderPage() {
  if (page == Page::Dashboard) drawDashboard();
  else if (page == Page::Camera) drawCameraPage();
  else drawSettingsPage();
}


static bool parseColor(const char *value, uint32_t &rgb) {
  if (!value) return false;
  const size_t len = strlen(value);
  if (len < 6) return false;
  const char *hex = value + len - 6;
  for (int i = 0; i < 6; ++i) if (!isxdigit((unsigned char)hex[i])) return false;
  rgb = strtoul(hex, nullptr, 16);
  return true;
}

static int estimateTotalSeconds(const String &filename) {
  const int dot = filename.lastIndexOf('.');
  const String base = dot > 0 ? filename.substring(0, dot) : filename;
  const int underscore = base.lastIndexOf('_');
  const String segment = underscore >= 0 ? base.substring(underscore + 1) : base;
  long total = 0, number = 0;
  bool haveNumber = false;
  for (unsigned i = 0; i < segment.length(); ++i) {
    const char c = segment[i];
    if (c >= '0' && c <= '9') { number = number * 10 + c - '0'; haveNumber = true; }
    else if (haveNumber && (c == 'd' || c == 'h' || c == 'm' || c == 's')) {
      total += number * (c == 'd' ? 86400L : c == 'h' ? 3600L : c == 'm' ? 60L : 1L);
      number = 0; haveNumber = false;
    } else return 0;
  }
  if (haveNumber || total < 60 || total > 30L * 86400L) return 0;
  return (int)total;
}

static int estimateRemaining(float elapsed, float progress, int estimateTotal) {
  float byFilename = estimateTotal > 0 ? estimateTotal - elapsed : -1;
  float byProgress = progress >= 0.05f ? elapsed * (1.0f - progress) / progress : -1;
  if (estimateTotal > 0 && byFilename <= 0) return byProgress >= 0 ? (int)byProgress : -1;
  if (byFilename >= 0 && byProgress >= 0) {
    const float weight = progress > 1.0f ? 1.0f : progress;
    return (int)((1.0f - weight) * byFilename + weight * byProgress);
  }
  if (byFilename >= 0) return (int)byFilename;
  if (byProgress >= 0) return (int)byProgress;
  return -1;
}

static String formatDuration(int seconds) {
  if (seconds < 0) return "--";
  const int days = seconds / 86400;
  const int hours = (seconds % 86400) / 3600;
  const int minutes = (seconds % 3600) / 60;
  char text[24];
  if (days > 0) snprintf(text, sizeof(text), "%dd %02d:%02d", days, hours, minutes);
  else snprintf(text, sizeof(text), "%02d:%02d", hours, minutes);
  return String(text);
}

static bool pollPrinter() {
  if (printerHost.isEmpty() || WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  const String url = "http://" + hostForMoonraker() +
    "/printer/objects/query?print_stats=state,filename,print_duration"
    "&virtual_sdcard=progress&extruder=temperature&heater_bed=temperature"
    "&box=map,same_material";
  http.setTimeout(2500);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;
  JsonObject st = doc["result"]["status"];
  if (st.isNull()) return false;

  const String raw = st["print_stats"]["state"] | "";
  if (raw == "printing") printer.state = "列印中";
  else if (raw == "paused") printer.state = "暫停";
  else if (raw == "complete") printer.state = "已完成";
  else if (raw == "error") printer.state = "錯誤";
  else if (raw == "standby") printer.state = "待機";
  else if (raw == "cancelled") printer.state = "已取消";
  else printer.state = raw.length() ? raw : "未知";

  printer.filename = String((const char *)(st["print_stats"]["filename"] | ""));
  printer.progress = constrain((float)(st["virtual_sdcard"]["progress"] | 0.0f), 0.0f, 1.0f);
  printer.nozzle = (int)((float)(st["extruder"]["temperature"] | -1.0f) + 0.5f);
  printer.bed = (int)((float)(st["heater_bed"]["temperature"] | -1.0f) + 0.5f);
  printer.elapsed = (int)(float)(st["print_stats"]["print_duration"] | -1.0f);
  printer.eta = (raw == "printing" || raw == "paused")
      ? estimateRemaining((float)printer.elapsed, printer.progress, estimateTotalSeconds(printer.filename)) : -1;

  for (int i = 0; i < 4; ++i) printer.slots[i] = SlotData();
  JsonArray sameMaterial = st["box"]["same_material"].as<JsonArray>();
  for (JsonVariant entryValue : sameMaterial) {
    JsonArray entry = entryValue.as<JsonArray>();
    const char *colorText = entry[1] | "";
    const char *materialText = entry[3] | "";
    uint32_t rgb = 0;
    if (!parseColor(colorText, rgb)) continue;
    for (JsonVariant slotValue : entry[2].as<JsonArray>()) {
      const char *slotCode = slotValue | "";
      if (strlen(slotCode) == 3 && slotCode[0] == 'T' && slotCode[1] == '1' && slotCode[2] >= 'A' && slotCode[2] <= 'D') {
        const int index = slotCode[2] - 'A';
        printer.slots[index].has = true;
        printer.slots[index].rgb = rgb;
        printer.slots[index].material = materialText;
      }
    }
  }
  printer.activeSlot = -1;
  if (raw == "printing" || raw == "paused") {
    const char *mapped = st["box"]["map"]["T1A"] | "";
    if (strlen(mapped) == 3 && mapped[0] == 'T' && mapped[1] == '1' && mapped[2] >= 'A' && mapped[2] <= 'D') {
      printer.activeSlot = mapped[2] - 'A';
    }
  }
  printer.online = true;
  return true;
}

static void handleRoot() {
  String html = "<meta charset='utf-8'><h2>K2 Monitor 設定</h2><form method='post' action='/save'>"
                "印表機 IP（可填 IP 或 IP:7125）：<input name='host' value='" + printerHost +
                "'><button>儲存</button></form><p>相機串流預設：<code>http://印表機IP:4408/go2rtc/api/stream.mjpeg?src=k2plus_mjpeg</code></p>";
  web.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  if (web.hasArg("host")) {
    printerHost = web.arg("host");
    printerHost.trim();
    prefs.putString("printer", printerHost);
  }
  web.sendHeader("Location", "/");
  web.send(303);
}

static void handleInfo() {
  JsonDocument doc;
  doc["printer"] = printerHost;
  doc["state"] = printer.state;
  doc["progress"] = printer.progress;
  doc["nozzle"] = printer.nozzle;
  doc["bed"] = printer.bed;
  doc["online"] = printer.online;
  String body;
  serializeJson(doc, body);
  web.send(200, "application/json", body);
}

static void onTouchInterrupt() {}

static void setupTouch() {
  touch.setPins(TP_RST, TP_INT);
  if (!touch.begin(Wire, TOUCH_ADDR, IIC_SDA, IIC_SCL)) {
    Serial.println("CST9217 touch init failed");
    return;
  }
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
}

static void handleTouch() {
  const uint8_t points = touch.getPoint(touchX, touchY, 2);
  if (!points || millis() - lastTouch < 350) return;
  lastTouch = millis();
  const int x = touchX[0], y = touchY[0];
  if (y >= 414 && y <= 460) {
    if (x < 155) page = Page::Dashboard;
    else if (x < 310) page = Page::Camera;
    else page = Page::Settings;
    renderPage();
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(IIC_SDA, IIC_SCL);

  // Original Waveshare 1.75 panel VDD is supplied by AXP2101 BLDO1.
  // TamaPoke (same exact board) enables this 3.3V rail before CO5300 init.
  Serial.println("[K2] Enabling AMOLED power via AXP2101 BLDO1...");
  if (panelPmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    panelPmu.setBLDO1Voltage(3300);
    panelPmu.enableBLDO1();
    delay(100);
    Serial.println("[K2] AXP2101 BLDO1 enabled at 3.3V");
  } else {
    Serial.println("[K2][ERROR] AXP2101 not detected; AMOLED power rail may be off");
  }

  prefs.begin("k2monitor", false);
  printerHost = prefs.getString("printer", "");
  cameraUrl = prefs.getString("camera", "");
  if (cameraUrl.isEmpty() && !printerHost.isEmpty()) {
    String ip = printerHost;
    const int colon = ip.indexOf(':');
    if (colon >= 0) ip = ip.substring(0, colon);
    cameraUrl = "http://" + ip + ":4408/go2rtc/api/stream.mjpeg?src=k2plus_mjpeg";
  }

  Serial.println("[K2] Starting CO5300 display...");
  if (!gfx->begin()) {
    Serial.println("[K2][ERROR] gfx->begin() failed - check CO5300/QSPI pins and GFX library");
    while (true) delay(1000);
  }

  gfx->setBrightness(180);
  gfx->fillScreen(BG);
  // Large, centered startup title sized for the 466x466 round AMOLED.
  drawCentered("K2 MONITOR", 142, 4, WHITE);
  gfx->drawRoundRect(82, 205, 302, 2, 1, BLUE);
  drawCentered("3D PRINTER MONITOR", 220, 2, MUTED);
  drawCentered("INITIALIZING...", 265, 2, BLUE);
  Serial.println("[K2] CO5300 display initialized");

  setupTouch();

  // Show connection progress while WiFiManager waits for credentials.
  drawCentered("CONNECTING WI-FI...", 310, 2, AMBER);
  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  const bool wifiConnected = wm.autoConnect("K2-Monitor-Setup");
  if (!wifiConnected) {
    Serial.println("[K2][WARN] Wi-Fi setup timed out or was cancelled");
    drawCentered("WI-FI NOT CONNECTED", 310, 2, RED);
    drawCentered("OPEN K2-Monitor-Setup", 342, 1, MUTED);
  } else {
    const String ssid = WiFi.SSID();
    Serial.print("[K2] Wi-Fi connected, SSID=");
    Serial.println(ssid);
    Serial.print("[K2] Wi-Fi connected, IP=");
    Serial.println(WiFi.localIP());

    // Briefly show the actual connected network name before opening the dashboard.
    gfx->fillRect(35, 295, 396, 100, BG);
    drawCentered("WI-FI CONNECTED", 300, 2, GREEN);
    drawCentered("SSID: " + ssid, 335, 2, WHITE);
    drawCentered(WiFi.localIP().toString(), 370, 2, MUTED);
    delay(1800);
  }

  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/api/info", HTTP_GET, handleInfo);
  web.begin();

  pollPrinter();
  renderPage();
}

void loop() {
  web.handleClient();
  handleTouch();
  cameraTick();
  if (millis() >= nextPoll && WiFi.status() == WL_CONNECTED) {
    nextPoll = millis() + 3000;
    const bool ok = pollPrinter();
    if (!ok) {
      printer.online = false;
      printer.state = "離線";
    }
    if (page == Page::Dashboard) drawDashboard();
  }
  delay(5);
}

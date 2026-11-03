#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

#define MAX_STOCKS 20
#define CONFIG_FILE "/config.json"
#define BL_PIN 5 // 背光腳位 (GPIO5 / D1)

TFT_eSPI tft;
ESP8266WebServer server(80);
WiFiManager wm;

struct StockData {
  char symbol[10];
  char name[20];
  float price;
  float change_percent;
  bool active;
};

StockData stocks[MAX_STOCKS];
int pageIntervalSec = 5; // 輪播秒數
int rotation = 3;        // 預設螢幕角度 (0:0deg, 1:90deg, 2:180deg, 3:270deg)
int totalActiveStocks = 0;
int currentPage = 0;
unsigned long lastPageSwitch = 0;

void loadConfig();
void saveConfig();
void countActiveStocks();
void drawStockPage(int pageIndex);
void handleRoot();
void handleSave();

// ---------------------------------------------------------------------------
// 讀取/儲存設定檔
// ---------------------------------------------------------------------------
void loadConfig() {
  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (f) {
      JsonDocument doc;
      if (!deserializeJson(doc, f)) {
        pageIntervalSec = constrain((int)(doc["interval"] | 5), 1, 60);
        rotation = constrain((int)(doc["rot"] | 3), 0, 3);
        JsonArray array = doc["stocks"];
        for (size_t i = 0; i < MAX_STOCKS && i < array.size(); i++) {
          strncpy(stocks[i].symbol, array[i]["sym"] | "", sizeof(stocks[i].symbol));
          strncpy(stocks[i].name, array[i]["name"] | "", sizeof(stocks[i].name));
        }
      }
      f.close();
      return;
    }
  }

  // 預設預載資料
  strcpy(stocks[0].symbol, "2330"); strcpy(stocks[0].name, "2330"); stocks[0].price = 2425.0; stocks[0].change_percent = 1.04;
  strcpy(stocks[1].symbol, "2454"); strcpy(stocks[1].name, "2454"); stocks[1].price = 4235.0; stocks[1].change_percent = 6.94;
  strcpy(stocks[2].symbol, "0050"); strcpy(stocks[2].name, "0050"); stocks[2].price = 108.65; stocks[2].change_percent = -1.92;
  strcpy(stocks[3].symbol, "2317"); strcpy(stocks[3].name, "2317"); stocks[3].price = 205.5; stocks[3].change_percent = 2.49;
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  JsonDocument doc;
  doc["interval"] = pageIntervalSec;
  doc["rot"] = rotation;
  JsonArray array = doc["stocks"].to<JsonArray>();

  for (int i = 0; i < MAX_STOCKS; i++) {
    JsonObject obj = array.add<JsonObject>();
    obj["sym"] = stocks[i].symbol;
    obj["name"] = stocks[i].name;
  }

  serializeJson(doc, f);
  f.close();
}

// ---------------------------------------------------------------------------
// 網頁設定介面 (含 20 股清單、輪播時間與螢幕角度切換)
// ---------------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='zh-TW'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP8266 股票看板設定</title>";
  html += "<style>";
  html += "body { font-family: sans-serif; background: #111; color: #eee; max-width: 440px; margin: auto; padding: 16px; }";
  html += ".card { background: #1d1d1d; padding: 15px; border-radius: 12px; margin-bottom: 15px; }";
  html += "h2 { color: #3ddc84; text-align: center; font-size: 20px; }";
  html += "label { display: block; color: #9aa; margin-top: 10px; font-size: 14px; }";
  html += "input, select { width: 100%; box-sizing: border-box; padding: 8px; font-size: 15px; background: #000; border: 1px solid #333; color: #fff; border-radius: 6px; }";
  html += ".row { display: flex; gap: 8px; margin-bottom: 6px; align-items: center; }";
  html += ".num { width: 25px; color: #666; text-align: right; }";
  html += ".btn { width: 100%; padding: 12px; background: #3ddc84; border: none; color: #000; font-size: 16px; font-weight: bold; margin-top: 15px; border-radius: 8px; cursor: pointer; }";
  html += "</style></head><body>";

  html += "<div class='card'><h2>📈 股票顯示器設定</h2>";
  html += "<form action='/save' method='POST'>";

  // 參數設定區
  html += "<label>⏱️ 頁面輪播時間 (秒)</label>";
  html += "<input type='number' name='interval' value='" + String(pageIntervalSec) + "' min='1' max='60' required>";

  html += "<label>🔄 螢幕旋轉角度</label>";
  html += "<select name='rot'>";
  html += "<option value='0'" + String(rotation == 0 ? " selected" : "") + ">0° (標準)</option>";
  html += "<option value='1'" + String(rotation == 1 ? " selected" : "") + ">90° (順時針)</option>";
  html += "<option value='2'" + String(rotation == 2 ? " selected" : "") + ">180° (倒立)</option>";
  html += "<option value='3'" + String(rotation == 3 ? " selected" : "") + ">270° (逆時針)</option>";
  html += "</select>";

  // 股票列表
  html += "<label style='margin-top:15px;'>📋 股票清單 (最多 20 支)</label>";
  for (int i = 0; i < MAX_STOCKS; i++) {
    html += "<div class='row'>";
    html += "<span class='num'>" + String(i + 1) + ".</span>";
    html += "<input type='text' name='sym" + String(i) + "' value='" + String(stocks[i].symbol) + "' placeholder='代碼'>";
    html += "<input type='text' name='name" + String(i) + "' value='" + String(stocks[i].name) + "' placeholder='名稱'>";
    html += "</div>";
  }

  html += "<input type='submit' value='💾 儲存並套用重啟' class='btn'>";
  html += "</form></div></body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("interval")) pageIntervalSec = constrain(server.arg("interval").toInt(), 1, 60);
  if (server.hasArg("rot")) rotation = constrain(server.arg("rot").toInt(), 0, 3);

  for (int i = 0; i < MAX_STOCKS; i++) {
    if (server.hasArg("sym" + String(i))) strncpy(stocks[i].symbol, server.arg("sym" + String(i)).c_str(), sizeof(stocks[i].symbol));
    if (server.hasArg("name" + String(i))) strncpy(stocks[i].name, server.arg("name" + String(i)).c_str(), sizeof(stocks[i].name));
  }

  saveConfig();

  String msg = "<html><body style='background:#111;color:#fff;text-align:center;'>";
  msg += "<h2>設定已儲存！</h2><p>ESP8266 正在重啟...</p></body></html>";
  server.send(200, "text/html", msg);
  
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// 螢幕繪圖
// ---------------------------------------------------------------------------
void countActiveStocks() {
  totalActiveStocks = 0;
  for (int i = 0; i < MAX_STOCKS; i++) {
    if (strlen(stocks[i].symbol) > 0) {
      stocks[i].active = true;
      totalActiveStocks++;
    } else {
      stocks[i].active = false;
    }
  }
}

void drawStockPage(int pageIndex) {
  tft.fillScreen(TFT_BLACK);

  int totalPages = (totalActiveStocks + 3) / 4;
  if (totalPages == 0) totalPages = 1;

  // 左側標籤
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("STOCKS", 5, 100, 2);

  char pageStr[10];
  sprintf(pageStr, "%d/%d", pageIndex + 1, totalPages);
  tft.drawString(pageStr, 5, 120, 2);

  int startIdx = pageIndex * 4;
  int startY = 10;
  int rowHeight = 58;

  for (int i = 0; i < 4; i++) {
    int stockIdx = startIdx + i;
    if (stockIdx >= totalActiveStocks) break;

    int y = startY + (i * rowHeight);

    // 第一行：代碼與價格
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(stocks[stockIdx].symbol, 40, y, 4);

    char priceStr[10];
    dtostrf(stocks[stockIdx].price, 4, (stocks[stockIdx].price >= 1000) ? 1 : 2, priceStr);
    tft.drawString(priceStr, 130, y, 4);

    // 第二行：名稱與漲跌幅
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString(stocks[stockIdx].name, 40, y + 26, 2);

    uint16_t color = (stocks[stockIdx].change_percent >= 0) ? TFT_RED : TFT_GREEN;
    char changeStr[15];
    if (stocks[stockIdx].change_percent >= 0) {
      sprintf(changeStr, "+%.2f%%", stocks[stockIdx].change_percent);
    } else {
      sprintf(changeStr, "%.2f%%", stocks[stockIdx].change_percent);
    }

    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(changeStr, 140, y + 26, 2);

    if (i < 3 && (stockIdx + 1) < totalActiveStocks) {
      tft.drawFastHLine(35, y + 50, 200, TFT_DARKGREY);
    }
  }
}

// ---------------------------------------------------------------------------
// 主程序 setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // 背光控制 (預設輸出 HIGH 點亮)
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // LittleFS 初始化
  LittleFS.begin();
  loadConfig();
  countActiveStocks();

  // ST7789 TFT 初始化
  tft.init();
  tft.setRotation(rotation); // 套用設定檔的角度
  tft.fillScreen(TFT_BLACK);

  // WiFiManager 配網
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ESP8266-Stock-Ticker")) {
    Serial.println("WiFi 配網逾時");
  }

  // Web Server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  drawStockPage(0);
}

void loop() {
  wm.process();
  server.handleClient();

  unsigned long intervalMs = pageIntervalSec * 1000;
  if (millis() - lastPageSwitch >= intervalMs) {
    lastPageSwitch = millis();

    int totalPages = (totalActiveStocks + 3) / 4;
    if (totalPages > 0) {
      currentPage = (currentPage + 1) % totalPages;
    } else {
      currentPage = 0;
    }

    drawStockPage(currentPage);
  }
}

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>

// ---------------------------------------------------------------------------
// 1. 硬體腳位定義與全域變數
// ---------------------------------------------------------------------------
#define TFT_CS    D8
#define TFT_RST   D4
#define TFT_DC    D3

#define MAX_STOCKS 20

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Font;
ESP8266WebServer server(80);

struct StockData {
  char symbol[10];
  char name[20];
  float price;
  float change_percent;
  bool active;
};

StockData stocks[MAX_STOCKS];
int pageIntervalSec = 5;       // 預設頁面輪播時間 (秒)
int totalActiveStocks = 0;
int currentPage = 0;
unsigned long lastPageSwitch = 0;

// 函式宣告
void loadConfig();
void saveConfig();
void countActiveStocks();
void drawStockPage(int pageIndex);
void handleRoot();
void handleSave();

// ---------------------------------------------------------------------------
// 2. LittleFS 檔案讀寫 (永久保存網頁設定數據)
// ---------------------------------------------------------------------------
void loadConfig() {
  if (!LittleFS.exists("/config.json")) {
    // 預設載入 4 檔測試資料
    strcpy(stocks[0].symbol, "2330"); strcpy(stocks[0].name, "台積電"); stocks[0].price = 2425.0; stocks[0].change_percent = 1.04;
    strcpy(stocks[1].symbol, "2454"); strcpy(stocks[1].name, "聯發科"); stocks[1].price = 4235.0; stocks[1].change_percent = 6.94;
    strcpy(stocks[2].symbol, "0050"); strcpy(stocks[2].name, "元大台灣50"); stocks[2].price = 108.65; stocks[2].change_percent = -1.92;
    strcpy(stocks[3].symbol, "2317"); strcpy(stocks[3].name, "鴻海"); stocks[3].price = 205.5; stocks[3].change_percent = 2.49;
    return;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return;

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (!error) {
    pageIntervalSec = doc["interval"] | 5;
    JsonArray array = doc["stocks"];
    for (size_t i = 0; i < MAX_STOCKS && i < array.size(); i++) {
      strncpy(stocks[i].symbol, array[i]["sym"] | "", sizeof(stocks[i].symbol));
      strncpy(stocks[i].name, array[i]["name"] | "", sizeof(stocks[i].name));
    }
  }
}

void saveConfig() {
  StaticJsonDocument<2048> doc;
  doc["interval"] = pageIntervalSec;
  JsonArray array = doc.createNestedArray("stocks");

  for (int i = 0; i < MAX_STOCKS; i++) {
    JsonObject obj = array.createNestedObject();
    obj["sym"] = stocks[i].symbol;
    obj["name"] = stocks[i].name;
  }

  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
  }
}

// ---------------------------------------------------------------------------
// 3. Web Server 網頁設定介面 (支援 20 股與秒數調整)
// ---------------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP8266 股票看板設定</title>";
  html += "<style>";
  html += "body { font-family: sans-serif; background: #121212; color: #fff; padding: 15px; }";
  html += ".card { background: #1e1e1e; padding: 15px; border-radius: 8px; max-width: 480px; margin: auto; }";
  html += "h2 { color: #4CAF50; text-align: center; margin-bottom: 15px; }";
  html += ".group { background: #252525; padding: 12px; border-radius: 6px; margin-bottom: 15px; }";
  html += ".row { display: flex; gap: 8px; margin-bottom: 8px; align-items: center; }";
  html += ".num { width: 25px; color: #888; text-align: right; }";
  html += "label { display: block; color: #aaa; margin-bottom: 5px; font-size: 14px; }";
  html += "input[type='text'], input[type='number'] { flex: 1; padding: 6px; background: #2a2a2a; border: 1px solid #444; color: #fff; border-radius: 4px; }";
  html += ".btn { width: 100%; padding: 12px; background: #4CAF50; border: none; color: white; font-size: 16px; margin-top: 15px; border-radius: 5px; cursor: pointer; }";
  html += "</style></head><body>";

  html += "<div class='card'><h2>📈 股票顯示器設定</h2>";
  html += "<form action='/save' method='POST'>";

  // 秒數區塊
  html += "<div class='group'><label><b>⏱️ 頁面輪播時間 (秒)：</b></label>";
  html += "<input type='number' name='interval' value='" + String(pageIntervalSec) + "' min='1' max='60' required></div>";

  // 20 檔股票列表區塊
  html += "<div class='group'><label><b>📋 股票清單 (最多 20 支)：</b></label>";
  for (int i = 0; i < MAX_STOCKS; i++) {
    html += "<div class='row'>";
    html += "<span class='num'>" + String(i + 1) + ".</span>";
    html += "<input type='text' name='sym" + String(i) + "' value='" + String(stocks[i].symbol) + "' placeholder='代碼'>";
    html += "<input type='text' name='name" + String(i) + "' value='" + String(stocks[i].name) + "' placeholder='名稱'>";
    html += "</div>";
  }
  html += "</div>";

  html += "<input type='submit' value='💾 儲存並套用重啟' class='btn'>";
  html += "</form></div></body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("interval")) {
    pageIntervalSec = server.arg("interval").toInt();
    if (pageIntervalSec < 1) pageIntervalSec = 5;
  }

  for (int i = 0; i < MAX_STOCKS; i++) {
    if (server.hasArg("sym" + String(i))) strncpy(stocks[i].symbol, server.arg("sym" + String(i)).c_str(), sizeof(stocks[i].symbol));
    if (server.hasArg("name" + String(i))) strncpy(stocks[i].name, server.arg("name" + String(i)).c_str(), sizeof(stocks[i].name));
  }

  saveConfig();

  String msg = "<html><body style='background:#121212;color:#fff;text-align:center;'>";
  msg += "<h2>設定已儲存！</h2><p>ESP8266 正在重啟...</p></body></html>";
  server.send(200, "text/html", msg);
  
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// 4. ST7789 (240x240) 繪圖與畫面輪播邏輯
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
  tft.fillScreen(0x000F); // 暗藍背景

  int totalPages = (totalActiveStocks + 3) / 4;
  if (totalPages == 0) totalPages = 1;

  // 左側垂直標籤與頁碼
  u8g2Font.setFont(u8g2_font_5x8_tr);
  u8g2Font.setForegroundColor(ST77XX_WHITE);
  u8g2Font.setCursor(5, 110);
  u8g2Font.print("STOCKS");

  char pageStr[10];
  sprintf(pageStr, "%d/%d", pageIndex + 1, totalPages);
  u8g2Font.setCursor(5, 130);
  u8g2Font.print(pageStr);

  int startIdx = pageIndex * 4;
  int startY = 18;
  int rowHeight = 55;

  for (int i = 0; i < 4; i++) {
    int stockIdx = startIdx + i;
    if (stockIdx >= totalActiveStocks) break;

    int y = startY + (i * rowHeight);

    // 第一行：代碼與價格
    u8g2Font.setFont(u8g2_font_6x12_tr);
    u8g2Font.setForegroundColor(ST77XX_WHITE);
    u8g2Font.setCursor(40, y);
    u8g2Font.print(stocks[stockIdx].symbol);

    u8g2Font.setFont(u8g2_font_7x14_tr);
    u8g2Font.setCursor(130, y);
    u8g2Font.print(stocks[stockIdx].price, (stocks[stockIdx].price >= 1000) ? 1 : 2);

    // 第二行：中文名稱與漲跌幅
    u8g2Font.setFont(u8g2_font_unifont_t_chinese1);
    u8g2Font.setForegroundColor(0x7BEF); // 淺灰
    u8g2Font.setCursor(40, y + 25);
    u8g2Font.print(stocks[stockIdx].name);

    uint16_t color = (stocks[stockIdx].change_percent >= 0) ? ST77XX_RED : ST77XX_GREEN;
    char changeStr[15];
    if (stocks[stockIdx].change_percent >= 0) {
      sprintf(changeStr, "+%.2f%%", stocks[stockIdx].change_percent);
    } else {
      sprintf(changeStr, "%.2f%%", stocks[stockIdx].change_percent);
    }

    u8g2Font.setForegroundColor(color);
    u8g2Font.setCursor(140, y + 25);
    u8g2Font.print(changeStr);

    if (i < 3 && (stockIdx + 1) < totalActiveStocks) {
      tft.drawFastHLine(35, y + 32, 200, 0x18E3);
    }
  }
}

// ---------------------------------------------------------------------------
// 5. Arduino 主流程
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // 初始化 LittleFS
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  countActiveStocks();

  // 初始化 ST7789 TFT
  tft.init(240, 240);
  tft.setRotation(3); // 旋轉 270 度符合側向機構
  tft.fillScreen(ST77XX_BLACK);
  u8g2Font.begin(tft);

  // 啟動 Wi-Fi
  WiFi.mode(WIFI_AP_STA);

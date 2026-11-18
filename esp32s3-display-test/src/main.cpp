#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// Waveshare ESP32-S3-Touch-AMOLED-1.75 (standard, non-C version).
// Pin assignments and display initialization follow the official
// Waveshare examples/arduino/01_HelloWorld sketch.
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

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

void setup()
{
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("[Waveshare Display Test] Starting");
    Serial.println("[Waveshare Display Test] Board: ESP32-S3-Touch-AMOLED-1.75 STD");
    Serial.println("[Waveshare Display Test] Initializing I2C (official example order)");

    Wire.begin(IIC_SDA, IIC_SCL);

    Serial.println("[Waveshare Display Test] Calling gfx->begin()");
    if (!gfx->begin()) {
        Serial.println("[Waveshare Display Test][ERROR] gfx->begin() failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("[Waveshare Display Test] CO5300 initialized");

    // Keep the official HelloWorld initialization sequence:
    // begin -> fillScreen -> setBrightness.
    gfx->fillScreen(RGB565_BLACK);
    gfx->setBrightness(128);

    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_GREEN);
    gfx->setCursor(72, 150);
    gfx->println("WAVESHARE TEST");

    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(112, 195);
    gfx->println("CO5300 OK");

    gfx->drawRect(32, 32, 402, 402, RGB565_BLUE);
    gfx->fillRect(52, 270, 90, 48, RGB565_RED);
    gfx->fillRect(188, 270, 90, 48, RGB565_GREEN);
    gfx->fillRect(324, 270, 90, 48, RGB565_BLUE);

    Serial.println("[Waveshare Display Test] Test pattern drawn");
}

void loop()
{
    delay(1000);
}

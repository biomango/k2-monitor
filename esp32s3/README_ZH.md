# K2 Monitor — ESP32-S3 圓形 AMOLED 移植版

此目錄是獨立的 ESP32-S3 移植目標，原本根目錄的 ESP8266 / ST7789 韌體保持不變。

## 硬體

- Waveshare ESP32-S3-Touch-AMOLED-1.75C
- 466 × 466 CO5300 QSPI AMOLED
- CST9217 電容觸控（I²C：SDA GPIO15、SCL GPIO14、INT GPIO11、RST GPIO2）
- ESP32-S3、32MB Flash、8MB PSRAM

## 建置

使用 PlatformIO：

```sh
pio run -d esp32s3
```

燒錄：

```sh
pio run -d esp32s3 -t upload
```

## 目前移植狀態

- [x] 新增獨立 ESP32-S3 PlatformIO target，不更動原 ESP8266 韌體。
- [x] CO5300 圓形 AMOLED 顯示初始化。
- [x] CST9217 觸控與總覽／相機／設定頁切換。
- [x] Wi-Fi 配網、Moonraker 基本狀態查詢、進度與噴頭／熱床溫度。
- [x] 保留 K2 Plus Moonraker 7125 埠查詢方式。
- [ ] 移植原版 CFS 四槽顏色／材質與作用槽顯示。
- [ ] 移植原版列印縮圖與 ETA 計算。
- [x] 加入 go2rtc MJPEG 串流接收與 JPEG 四分之一縮放解碼（尚待實機驗證更新率及相容性）。
- [ ] 實機驗證觸控方向、顯示亮度、記憶體及攝影機更新率。

## K2 Plus 攝影機

相機頁預留的區域網路串流 URL：

```
http://<印表機IP>:4408/go2rtc/api/stream.mjpeg?src=k2plus_mjpeg
```

此 URL 需要 K2 系列 Helper Script 的 Camera Support／go2rtc 設定。若未安裝，先用瀏覽器在同一區域網路測試：

```
http://<印表機IP>:4408/go2rtc/api/frame.jpeg?src=k2plus
```

目前程式已加入 MJPEG multipart 讀取及 JPEG 四分之一縮放解碼，畫面更新只在相機頁進行。K2 Plus Helper Script 的串流實際格式、相機影像方向、解碼速度與長時間穩定性仍需實機驗證；串流尚未經過 CI 或硬體驗證。

## 注意

目前 ESP32-S3 版本是移植中的開發版，不是已完成的替代韌體。請先不要覆蓋原本 ESP8266 裝置或刪除原分支。

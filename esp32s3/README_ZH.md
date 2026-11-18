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
- [ ] 完成 go2rtc MJPEG 串流接收、JPEG 解碼與 466×466 畫面裁切。
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

MJPEG 即時串流是目標方案；目前此移植初版先完成面板、觸控及列印狀態骨架，尚未宣稱相機影像已能顯示。相機串流需以獨立讀取任務處理，避免阻塞 Moonraker 三秒輪詢。

## 注意

目前 ESP32-S3 版本是移植中的開發版，不是已完成的替代韌體。請先不要覆蓋原本 ESP8266 裝置或刪除原分支。

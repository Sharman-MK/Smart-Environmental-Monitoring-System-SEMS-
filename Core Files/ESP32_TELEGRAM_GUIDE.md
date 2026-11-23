# SEMS &mdash; ESP32, Sensors & Telegram Bot Integration Guide

This guide walks you through setting up your **ESP32** microcontroller with the **DHT22** and **MQ-135** sensors, serving the web dashboard, and integrating the real-time **Telegram incident bot** using your registered bot token.

---

## 1. Telegram Bot Details

* **Bot Name:** SEMS_BotSys
* **Username:** [@SemibotSys_Bot](https://t.me/SemibotSys_Bot)
* **Bot Token:** `9063237995:AAFZgh6AyIRXoug15AddU4KaJa9wjeFHGxc`
* **Direct Chat Link:** [https://t.me/SemibotSys_Bot](https://t.me/SemibotSys_Bot)

### How to Link Your Telegram Chat:
1. Open Telegram on your phone or computer.
2. Search for `@SemibotSys_Bot` (or open [https://t.me/SemibotSys_Bot](https://t.me/SemibotSys_Bot)).
3. Press **Start** (or send `/start`).
4. The ESP32 will auto-detect your chat ID, record it in memory, and reply:
   ```text
   🌿 SEMS - Smart Environmental Monitoring System
   Node: ESP32-NODE-01 is online!
   ✅ Linked to your chat! Automatic environmental incident alerts will be delivered here.
   ```
5. You can now use:
   * `/status` &mdash; View current temperature, humidity, air quality ADC & ppm
   * `/temp` &mdash; Current temperature reading
   * `/hum` &mdash; Current relative humidity reading
   * `/air` &mdash; MQ-135 analog ADC and ppm concentration
   * `/limits` &mdash; View current threshold limits
   * `/test` &mdash; Trigger a test notification

---

## 2. Hardware Wiring & Pin Mapping

| Sensor | Sensor Pin | ESP32 Pin | Voltage | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **DHT22** | Pin 1 (VCC) | **3.3V** | 3.3V DC | Main power |
| **DHT22** | Pin 2 (DATA) | **GPIO 26** (or GPIO 4) | 3.3V Logic | Add a 4.7k&Omega;&ndash;10k&Omega; pull-up resistor to 3.3V |
| **DHT22** | Pin 4 (GND) | **GND** | 0V | Common Ground |
| **MQ-135** | VCC | **VIN / 5V** | 5.0V DC | Required for internal ceramic heater |
| **MQ-135** | GND | **GND** | 0V | Common Ground |
| **MQ-135** | AOUT | **GPIO 36 / VP** (ADC1) | 0&ndash;3.3V | ESP32 ADC1 input (attenuation set to 11dB) |

> [!NOTE]
> MQ-135 analog output connects to **ADC1** pins (such as GPIO 36 / VP or GPIO 34). Do not use ADC2 pins (GPIO 0, 2, 4, 12-15, 25-27) for analog reads when Wi-Fi is active, as Wi-Fi uses ADC2 internally.

---

## 3. Flashing Firmware via Arduino IDE

### Required Arduino Libraries:
In Arduino IDE, go to **Sketch &rarr; Include Library &rarr; Manage Libraries...** and install:
1. **DHT sensor library** (by Adafruit)
2. **Adafruit Unified Sensor** (by Adafruit)
3. *(Optional)* **U8g2** or **Adafruit_SSD1306** (if you have an I2C OLED display attached)

### Board Setup:
1. Connect your ESP32 to your PC via Micro-USB or USB-C.
2. In Arduino IDE, select:
   * **Tools &rarr; Board &rarr; ESP32 Arduino &rarr; DOIT ESP32 DEVKIT V1** (or ESP32 Dev Module)
   * **Tools &rarr; Upload Speed &rarr; 921600** (or 115200)
   * **Tools &rarr; Port &rarr; [Your COM port]**
3. Open `SEMS_ESP32.ino` located in `c:\Users\Asus\Desktop\IDP\SEMS_ESP32.ino`.
4. Update your Wi-Fi credentials near the top of the file:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   ```
5. Click **Upload**.
6. Open **Serial Monitor** at **115200 baud** to view the assigned IP address (e.g., `http://192.168.1.105`) and startup logs.

---

## 4. Accessing the Web Dashboard

### A. Direct from ESP32:
Open your web browser and navigate to:
* `http://192.168.1.xxx` (the IP shown in Serial Monitor)
* or `http://sems.local`

### B. From the standalone `index.html` file:
1. Open `index.html` in Chrome/Edge/Firefox.
2. In the top bar, click the **ESP32: Connect** pill and type your ESP32's IP address (e.g. `http://192.168.1.105`).
3. The dashboard will automatically pull live data from the ESP32's `/api/data` REST endpoint, update the charts, and allow you to configure thresholds and trigger real Telegram alerts!


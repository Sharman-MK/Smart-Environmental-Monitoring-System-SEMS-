/*
 * ======================================================================================
 *  SEMS - Smart Environmental Monitoring System
 *  ESP32 Integrated Firmware: Sensors (DHT22 + MQ-135) + OLED Display (U8g2)
 *                             + Full Web Dashboard (Port 80) + Telegram Bot (@SemibotSys_Bot)
 * ======================================================================================
 *  Hardware Connections:
 *   - DHT22 (Temp & Humidity): Data pin -> GPIO 26 (with 10k pull-up to 3.3V)
 *   - MQ-135 (Gas & Air Quality): Analog pin -> GPIO 36 (VP / ADC1_CH0)
 *   - SSD1306 128x64 OLED: I2C (SDA -> GPIO 21, SCL -> GPIO 22)
 *   - Physical Alert Buzzer: Signal pin -> GPIO 32 (GND -> GND)
 *
 *  Telegram Bot Configuration:
 *   - Bot Username: @SemibotSys_Bot
 *   - Bot Token:    9063237995:AAFZgh6AyIRXoug15AddU4KaJa9wjeFHGxc
 *   - Pair chat:    Send /start to @SemibotSys_Bot in Telegram!
 *
 *  Web Dashboard & REST API:
 *   - Full Dashboard: http://<ESP32_IP>/ or http://sems.local/
 *   - REST JSON:      http://<ESP32_IP>/api/data
 * ======================================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "DHT.h"
#include <Preferences.h>
#include <math.h>

// ======================================================================================
// 1. WI-FI & TELEGRAM BOT CONFIGURATION
// ======================================================================================
const char* WIFI_SSID = "Redmi Pad SE 8.7 4G";          // <--- Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "12345678";      // <--- Replace with your Wi-Fi Password

// Telegram Bot credentials
#define BOT_TOKEN "9063237995:AAFZgh6AyIRXoug15AddU4KaJa9wjeFHGxc"
String g_chatId = ""; // Auto-linked when you send /start to @SemibotSys_Bot

// Hostname for mDNS (http://sems.local)
const char* MDNS_NAME = "sems";

// ======================================================================================
// 2. SENSOR & HARDWARE PIN DEFINITIONS
// ======================================================================================
#define DHTPIN     26       // DHT22 connected to GPIO 26
#define DHTTYPE    DHT22
#define MQ135_PIN  36       // VP pin on ESP32 (GPIO 36 / ADC1_CH0)
#define BUZZER_PIN 32       // Physical alert buzzer connected to GPIO 32

// MQ-135 Gas Calculation Constants
#define RL_VALUE           10.0f   // Load resistor on PCB in kOhm
float R0_CLEAN_AIR =       76.63f; // Baseline resistance in clean air (kOhm) - recalibrate via Serial 'c'
const float GAS_A = 110.47f;       // CO2/Air Contaminants scaling factor
const float GAS_B = -2.862f;       // Exponent curve
const float ATMOSPHERIC_CO2_BASELINE = 400.0f;

// MQ-135 measurement settings (same method as Esp32GasTest.ino)
const float    ADC_VREF          = 3.3f;  // ESP32 ADC reference / sensor circuit voltage
const float    DIVIDER_RATIO     = 1.0f;  // AOUT -> ADC divider ratio (1.0 = none, 0.667 = 10k/20k)
const uint16_t MQ_SAMPLES        = 8;     // Samples averaged per reading (~16ms)
const uint16_t MQ_SAMPLE_DELAY   = 2;     // ms between samples
const uint16_t MQ_CALIB_SAMPLES  = 50;    // Readings averaged during R0 calibration

// ======================================================================================
// 3. DRIVER OBJECTS & STATE
// ======================================================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Preferences prefs;

// Sensor Variables
float g_temp = 0.0f;
float g_hum = 0.0f;
float g_ppm = 0.0f;
float g_mq135Rs = 0.0f;     // Sensor resistance (kOhm)
float g_mq135Ratio = 0.0f;  // Rs / R0
float g_mq135Voltage = 0.0f;
int   g_mq135Raw = 0;
bool  g_dhtSuccess = false;
bool  g_demoSimulation = false;

// Alert Thresholds (Synchronized with index.html & Telegram)
float g_tempLimit = 30.0f; // Celsius
int   g_airLimit  = 1000;  // Gas concentration limit in ppm (0 - 5000)

// System Timers & Tracking
enum DkScreenId { DK_SCREEN_0, DK_SCREEN_1 };
static DkScreenId g_screen = DK_SCREEN_0;
static bool g_dirty = true;
static uint32_t g_lastDrawMs = 0;
static uint32_t g_lastMqReadMs = 0;
static uint32_t g_lastDhtReadMs = 0;
volatile bool g_pendingTempAlert = false;
volatile bool g_pendingAirAlert  = false;
volatile bool g_pendingTestAlert = false;
volatile bool g_pendingSimAlert  = false;
bool g_buzzerBeepOn = false;
uint32_t g_lastBuzzerToggleMs = 0;
static uint32_t g_lastTempAlertMs = 0;
static uint32_t g_lastAirAlertMs = 0;
static const unsigned long g_alertCooldownMs = 60000; // 60s cooldown between alerts
unsigned long g_packetCounter = 0;
unsigned long g_alertCounter = 0;
unsigned long g_bootTimeMs = 0;

// OLED Custom Bitmaps
const unsigned char icon_temp_el_mwkfd4o50_xbm[32] PROGMEM = {
  0xE0, 0x01, 0x60, 0x07, 0x20, 0x3B, 0x60, 0x22, 0x20, 0x3A, 0xE0, 0x0A, 0xE0, 0x3A, 0xA0, 0x02, 
  0xA0, 0x3A, 0xA0, 0x03, 0xB0, 0x0D, 0xF0, 0x05, 0xF0, 0x05, 0x20, 0x02, 0xC0, 0x01, 0x00, 0x00
};

const unsigned char icon_nuke_el_heob4nwqe_xbm[32] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x10, 0x08, 0x18, 0x1C, 0x3C, 0x3C, 0x7E, 0x7E, 0x3E, 0x7C, 0xBE, 0x7D, 
  0x80, 0x01, 0x00, 0x00, 0x80, 0x01, 0xC0, 0x03, 0xE0, 0x07, 0xE0, 0x07, 0xE0, 0x07, 0x00, 0x00
};

// ======================================================================================
// EMBEDDED SEMS LOGO (PROGMEM) & FULL INDEX.HTML DASHBOARD (PROGMEM)
// ======================================================================================
const uint8_t SEMS_LOGO_JPG[] PROGMEM = {
  0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x60,
  0x00, 0x60, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
  0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
  0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20,
  0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29, 0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27,
  0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x09, 0x09,
  0x09, 0x0C, 0x0B, 0x0C, 0x18, 0x0D, 0x0D, 0x18, 0x32, 0x21, 0x1C, 0x21, 0x32, 0x32, 0x32, 0x32,
  0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
  0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
  0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0xFF, 0xC0,
  0x00, 0x11, 0x08, 0x00, 0xA0, 0x00, 0xA0, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
  0x01, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
  0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
  0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
  0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23,
  0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17,
  0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
  0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
  0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
  0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
  0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
  0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
  0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
  0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00, 0x1F, 0x01, 0x00, 0x03,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
  0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
  0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
  0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
  0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27,
  0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
  0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
  0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
  0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
  0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2,
  0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
  0xFA, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF7,
  0xEA, 0x28, 0xA2, 0x80, 0x0A, 0x28, 0xA2, 0x80, 0x0A, 0x28, 0xAA, 0xBA, 0x8E, 0xA3, 0x67, 0xA4,
  0x69, 0xD3, 0xDF, 0xDF, 0xDC, 0x25, 0xBD, 0xAC, 0x08, 0x5E, 0x49, 0x1C, 0xF0, 0xA3, 0xFC, 0xF6,
  0xEF, 0x40, 0x16, 0x24, 0x91, 0x22, 0x8D, 0xA4, 0x91, 0xD5, 0x11, 0x41, 0x66, 0x66, 0x38, 0x00,
  0x0E, 0xA4, 0x9A, 0xF3, 0x2D, 0x67, 0xE2, 0xDF, 0xDA, 0xB5, 0x16, 0xD1, 0xFC, 0x11, 0xA5, 0x49,
  0xAE, 0xEA, 0x03, 0x86, 0x99, 0x41, 0x10, 0x47, 0xEF, 0x9E, 0xE3, 0xDF, 0x20, 0x7B, 0xD7, 0x2B,
  0xAD, 0xEB, 0xBA, 0x87, 0xC4, 0x72, 0x6E, 0xAF, 0x26, 0xB8, 0xD2, 0x3C, 0x18, 0x92, 0x14, 0x86,
  0x18, 0xC7, 0xFA, 0x46, 0xA2, 0xC0, 0xF4, 0x03, 0xBF, 0xFE, 0x82, 0xBE, 0xE6, 0x95, 0xE6, 0x36,
  0xD6, 0x3F, 0xD9, 0x7A, 0x7D, 0xB2, 0x69, 0xD6, 0x03, 0xFE, 0x5C, 0xAD, 0x8F, 0x2D, 0xEF, 0x34,
  0x9D, 0x5D, 0xBF, 0xCF, 0x15, 0xD3, 0x42, 0x84, 0xAA, 0x4F, 0x92, 0x2A, 0xEF, 0xF0, 0x5E, 0xA7,
  0x9D, 0x8C, 0xCC, 0x69, 0xE1, 0x95, 0x9E, 0xAF, 0xB7, 0xF5, 0xB1, 0x1E, 0xA9, 0x69, 0xAD, 0xEB,
  0x6A, 0xC9, 0xE3, 0x4F, 0x1B, 0x04, 0x46, 0xE5, 0xB4, 0xBD, 0x22, 0x21, 0x26, 0x3D, 0x98, 0x8C,
  0x28, 0x3F, 0x52, 0xD5, 0x98, 0xBA, 0x0F, 0x80, 0xAD, 0x7E, 0x51, 0xA4, 0x6A, 0xF7, 0x78, 0xFE,
  0x29, 0xEF, 0x84, 0x64, 0xFE, 0x08, 0x38, 0xA9, 0x26, 0x8A, 0x54, 0x18, 0x2B, 0xB5, 0x7F, 0xBA,
  0xA3, 0x02, 0xBA, 0x8D, 0x32, 0xC2, 0xD7, 0x4F, 0x8E, 0x18, 0xED, 0x6C, 0x13, 0x56, 0xD5, 0x25,
  0x8C, 0x4A, 0xFB, 0x88, 0x31, 0x40, 0xA7, 0xA0, 0xF4, 0xCD, 0x7B, 0x31, 0xC0, 0xC2, 0x94, 0x6F,
  0x51, 0xDF, 0xC9, 0x68, 0xBE, 0xF3, 0xC5, 0xFE, 0xD3, 0xC4, 0x55, 0x97, 0xBB, 0x68, 0xFC, 0xAE,
  0xCE, 0x42, 0x5D, 0x1B, 0xC0, 0x53, 0x8C, 0x36, 0x8B, 0xAC, 0xDA, 0x0F, 0xEF, 0xDB, 0x5E, 0x89,
  0x48, 0xFC, 0x1C, 0x56, 0x96, 0x88, 0x35, 0x9D, 0x27, 0x64, 0x7E, 0x08, 0xF1, 0xAA, 0x5C, 0xA8,
  0xFB, 0xBA, 0x46, 0xAF, 0x18, 0x89, 0x8F, 0xFB, 0x2B, 0xBB, 0xE5, 0x27, 0xFD, 0xD2, 0xB5, 0xD1,
  0xDF, 0x59, 0xC3, 0x7C, 0xE2, 0xD3, 0x54, 0xD2, 0xE3, 0xD3, 0x2E, 0xE4, 0xFF, 0x00, 0x51, 0x75,
  0x0E, 0x3C, 0xB6, 0x6F, 0x46, 0xC7, 0x15, 0xC4, 0xCF, 0x64, 0x92, 0xE6, 0x39, 0x51, 0x5F, 0x07,
  0x1F, 0xFE, 0xA3, 0x57, 0xF5, 0x1A, 0x55, 0xA3, 0xEE, 0xDD, 0x3F, 0xBD, 0x16, 0xB3, 0x4A, 0xB4,
  0x65, 0xFB, 0xCD, 0x57, 0xDC, 0xCF, 0x4D, 0xF0, 0xF7, 0xC5, 0x7B, 0x79, 0xB5, 0x11, 0xA2, 0x78,
  0xB2, 0xC2, 0x4D, 0x03, 0x58, 0xE0, 0x05, 0x9F, 0x22, 0x19, 0x09, 0xE9, 0xB5, 0x8F, 0x4C, 0xF6,
  0xCF, 0x1E, 0xE6, 0xBD, 0x1A, 0xBE, 0x73, 0x7B, 0xC2, 0xFA, 0x7A, 0xE9, 0xBA, 0xFD, 0xB3, 0x6B,
  0x3A, 0x32, 0xFD, 0xD0, 0xE7, 0xFD, 0x26, 0xD7, 0xFD, 0xA8, 0xA4, 0xEB, 0xC7, 0xA1, 0xE2, 0xBA,
  0x0F, 0x0C, 0x78, 0xBA, 0xF3, 0xC0, 0x86, 0xD2, 0xDF, 0x52, 0xBE, 0x6D, 0x5B, 0xC1, 0xB7, 0x6D,
  0xE5, 0xD9, 0xEA, 0x98, 0x25, 0xED, 0x1B, 0xFE, 0x79, 0xCA, 0x3A, 0x8C, 0x7A, 0x7E, 0x23, 0x8E,
  0x2B, 0xC8, 0xC4, 0xE0, 0xA7, 0x45, 0xEA, 0x8F, 0x73, 0x0B, 0x8D, 0xA7, 0x88, 0x8D, 0xE2, 0xCF,
  0x6C, 0xA2, 0x9B, 0x1C, 0x89, 0x2C, 0x4B, 0x24, 0x6E, 0xAF, 0x1B, 0xA8, 0x65, 0x65, 0x39, 0x0C,
  0x0F, 0x42, 0x0F, 0x71, 0x4E, 0xAE, 0x13, 0xB0, 0x28, 0xA2, 0x8A, 0x00, 0x28, 0xA2, 0x8A, 0x00,
  0x28, 0xA2, 0x8A, 0x00, 0x28, 0xA2, 0x8A, 0x00, 0x42, 0x42, 0xA9, 0x66, 0x20, 0x00, 0x32, 0x49,
  0xED, 0x5E, 0x1F, 0xAD, 0xEA, 0xA7, 0xE2, 0x77, 0x88, 0x66, 0x06, 0x49, 0x17, 0xC1, 0x9A, 0x44,
  0xB8, 0x60, 0x87, 0x69, 0xBF, 0x9C, 0x74, 0x00, 0xFA, 0x7B, 0xF6, 0x5E, 0x7A, 0xB0, 0xAE, 0x93,
  0xE2, 0xF7, 0x88, 0xEE, 0xA2, 0xB3, 0xB3, 0xF0, 0x8E, 0x8E, 0xD9, 0xD5, 0x75, 0xA6, 0xF2, 0xDB,
  0x07, 0x06, 0x38, 0x49, 0xC1, 0x27, 0xD3, 0x3C, 0x8C, 0xFA, 0x06, 0xAC, 0x26, 0xB1, 0x87, 0x4E,
  0xD3, 0x6D, 0x74, 0x2D, 0x37, 0xFE, 0x3D, 0x2D, 0x53, 0x0C, 0xF8, 0xC1, 0x90, 0x9E, 0x59, 0x8F,
  0xBB, 0x1E, 0x7E, 0x98, 0x14, 0xED, 0x39, 0x4E, 0x34, 0x69, 0x7C, 0x73, 0xDB, 0xC9, 0x2D, 0xE4,
  0xFD, 0x3A, 0x77, 0x76, 0x47, 0x0E, 0x3B, 0x14, 0xA8, 0x53, 0x72, 0x29, 0x5E, 0xDD, 0x3D, 0xC5,
  0xD8, 0x91, 0x36, 0xA6, 0xC4, 0x11, 0xC4, 0xB1, 0x8D, 0xA9, 0x0A, 0x0E, 0x02, 0x20, 0xEC, 0x05,
  0x6E, 0xF8, 0x4F, 0x46, 0x8A, 0xF2, 0xE5, 0x7C, 0xFE, 0x13, 0x39, 0x6F, 0xA5, 0x51, 0x5D, 0x22,
  0x68, 0x42, 0x3C, 0x91, 0xB2, 0xAB, 0xF2, 0xA4, 0x8C, 0x03, 0x5D, 0x0E, 0x93, 0x6E, 0x3E, 0xCF,
  0x24, 0x4D, 0x6B, 0xF6, 0x95, 0x74, 0x20, 0xC0, 0x1B, 0x6F, 0x99, 0xED, 0x9E, 0xD5, 0xF4, 0x53,
  0xA7, 0x4F, 0x0B, 0x85, 0xF6, 0x74, 0x3E, 0xFE, 0xAD, 0xF7, 0x7E, 0x67, 0xCB, 0x60, 0x69, 0xCE,
  0xAE, 0x25, 0x4E, 0xB2, 0xDD, 0x9D, 0x15, 0xE6, 0x89, 0xA1, 0x48, 0xF2, 0xEF, 0x82, 0xC4, 0xC5,
  0xE5, 0x85, 0x54, 0x0A, 0x03, 0x87, 0xC9, 0xC9, 0xDD, 0x9E, 0x7A, 0x63, 0x18, 0xEA, 0x0D, 0x70,
  0x16, 0x20, 0x5A, 0xCF, 0x3E, 0x96, 0xF7, 0x0F, 0x0D, 0xA1, 0x90, 0xB3, 0x49, 0x02, 0xFE, 0xF2,
  0x6C, 0x70, 0xA8, 0x0F, 0xBD, 0x6F, 0x5C, 0xE9, 0xBB, 0x6F, 0x8C, 0x8B, 0xE1, 0x9B, 0x15, 0xFD,
  0xF2, 0xB6, 0xF3, 0x7D, 0x96, 0xFF, 0x00, 0x5D, 0x70, 0x77, 0x63, 0x3F, 0xED, 0x6E, 0x03, 0xD6,
  0x52, 0x3F, 0x86, 0xB3, 0xD2, 0xD1, 0xD6, 0xE6, 0x39, 0x63, 0x51, 0x1B, 0xA9, 0x0C, 0xA1, 0x79,
  0x0A, 0x47, 0xA7, 0xAE, 0x2B, 0x8B, 0x2E, 0xAB, 0x26, 0xE5, 0x19, 0x3B, 0xAB, 0x1E, 0xD6, 0x6F,
  0x42, 0x2A, 0x30, 0x9C, 0x15, 0x9D, 0xFF, 0x00, 0x02, 0x1B, 0xC8, 0x49, 0xB4, 0x6B, 0x4B, 0x49,
  0x26, 0x92, 0xDA, 0x43, 0x96, 0x86, 0xE8, 0x65, 0xE0, 0x75, 0x3D, 0x8D, 0x63, 0xC9, 0xA6, 0x3A,
  0x76, 0xAD, 0xF9, 0xEE, 0xFE, 0xC4, 0xF7, 0x36, 0xE6, 0x3D, 0xF2, 0x04, 0x12, 0x99, 0x0B, 0x6D,
  0x20, 0xB7, 0x27, 0x8E, 0x86, 0x9D, 0xE1, 0xC8, 0xE5, 0xF1, 0x0D, 0xB4, 0x8E, 0xE0, 0x2F, 0x97,
  0x31, 0x8D, 0x9F, 0x19, 0xC0, 0xC0, 0x3C, 0xFB, 0xF3, 0x5D, 0x94, 0xF3, 0x18, 0x46, 0x6E, 0x9F,
  0xE2, 0x79, 0xD3, 0xCA, 0x6A, 0x4E, 0x92, 0xAA, 0xF7, 0x7D, 0x0E, 0x5B, 0xEC, 0x8C, 0x0F, 0xDD,
  0x35, 0x59, 0x15, 0xB4, 0x93, 0x72, 0xE2, 0xD4, 0x5D, 0xE9, 0x57, 0x4B, 0xB7, 0x51, 0xD3, 0xDB,
  0xEE, 0xCA, 0x9F, 0xDF, 0x5F, 0x46, 0x5E, 0xA0, 0x8E, 0x9F, 0x9D, 0x7A, 0xD5, 0xE6, 0x9F, 0x04,
  0x9A, 0x64, 0x56, 0x0A, 0x14, 0x08, 0xC0, 0xC3, 0x6D, 0xEF, 0xEB, 0x5C, 0xCB, 0xE9, 0xCD, 0x0C,
  0xC1, 0x4A, 0x61, 0x81, 0xE0, 0xE3, 0xAD, 0x6E, 0xB1, 0x54, 0xF1, 0x10, 0x71, 0x92, 0x31, 0x8E,
  0x12, 0xB6, 0x0E, 0xAA, 0x9C, 0x25, 0x7F, 0xEB, 0x62, 0x97, 0x80, 0x35, 0xF6, 0xF0, 0xB6, 0xAD,
  0x6B, 0xE1, 0x7B, 0xCB, 0xC6, 0xBA, 0xD0, 0xB5, 0x25, 0x32, 0xE8, 0x37, 0xEF, 0xE9, 0x9E, 0x60,
  0x6F, 0x46, 0x07, 0x8C, 0x76, 0x3F, 0x51, 0x8F, 0x5E, 0xAF, 0x01, 0xBF, 0xD1, 0x10, 0xCB, 0x2F,
  0x87, 0x26, 0x6F, 0x22, 0xCB, 0x53, 0x7F, 0xB4, 0x69, 0x53, 0x93, 0x8F, 0xB1, 0xDE, 0xAF, 0x40,
  0x0F, 0x65, 0x6E, 0x14, 0xFD, 0x47, 0xA5, 0x7A, 0x8F, 0xC3, 0xBF, 0x14, 0x4B, 0xE2, 0x7F, 0x0C,
  0xAB, 0x5E, 0xA9, 0x8F, 0x56, 0xB1, 0x73, 0x69, 0x7F, 0x13, 0x0C, 0x15, 0x95, 0x78, 0x27, 0x1E,
  0xFD, 0x7E, 0xB9, 0x1D, 0xAB, 0xC0, 0xC5, 0x51, 0xF6, 0x72, 0xBA, 0x3E, 0xAF, 0x0D, 0x59, 0x55,
  0x85, 0xCE, 0xB6, 0x8A, 0x28, 0xAE, 0x53, 0xA4, 0x28, 0xA2, 0x8A, 0x00, 0x28, 0xA2, 0x8A, 0x00,
  0x29, 0xB2, 0x48, 0x91, 0x46, 0xD2, 0x48, 0xC1, 0x51, 0x01, 0x66, 0x62, 0x78, 0x00, 0x75, 0x34,
  0xEA, 0xE2, 0xBE, 0x2B, 0xEA, 0xCF, 0xA5, 0x7C, 0x3C, 0xD4, 0x7C, 0x96, 0x2B, 0x35, 0xD8, 0x16,
  0xA8, 0x47, 0x5F, 0x9F, 0xEF, 0x7F, 0xE3, 0xA1, 0xA9, 0xC5, 0x73, 0x34, 0x85, 0x26, 0xA2, 0xAE,
  0xCF, 0x34, 0xF0, 0xED, 0xF3, 0x78, 0x9F, 0xC5, 0xDA, 0xF7, 0x8D, 0x6E, 0x01, 0x31, 0x97, 0x36,
  0x7A, 0x7A, 0x9F, 0xE1, 0x4C, 0x72, 0x47, 0xBE, 0xCC, 0x0F, 0xAC, 0x86, 0xBB, 0xCD, 0x2B, 0x47,
  0x13, 0x10, 0x27, 0x57, 0x2E, 0xE7, 0x73, 0x05, 0x1D, 0x49, 0xF7, 0xF4, 0xED, 0x58, 0xFE, 0x05,
  0xF0, 0xFB, 0x5B, 0xE9, 0x7A, 0x66, 0x9C, 0x53, 0x0B, 0x6F, 0x00, 0x9E, 0x71, 0xEB, 0x23, 0xFC,
  0xD8, 0xFD, 0x40, 0xFF, 0x00, 0x80, 0xD6, 0x77, 0x8E, 0x7C, 0x57, 0x79, 0x77, 0xAD, 0x8F, 0x08,
  0xF8, 0x74, 0x4C, 0xCC, 0xAC, 0x61, 0xBA, 0x10, 0x2E, 0x5A, 0xE1, 0xF1, 0xCC, 0x60, 0xF5, 0x0A,
  0x3F, 0x88, 0xE4, 0x67, 0x9C, 0xF1, 0x4F, 0x01, 0x3E, 0x6A, 0xB5, 0x6B, 0xC7, 0x76, 0xF9, 0x57,
  0x94, 0x63, 0xBF, 0xDE, 0xEF, 0xF7, 0x23, 0xC7, 0xAB, 0x4B, 0xDB, 0xD4, 0xE6, 0x9F, 0xC2, 0xBF,
  0x16, 0x76, 0xD7, 0x1E, 0x20, 0xD3, 0x57, 0x5A, 0xD2, 0x74, 0xA1, 0x17, 0xDA, 0x13, 0x51, 0x32,
  0xC6, 0x97, 0x4A, 0xC3, 0xCA, 0x56, 0x40, 0x3E, 0x55, 0xFE, 0xFF, 0x00, 0x3C, 0x1C, 0x70, 0x3A,
  0x67, 0x3C, 0x54, 0xE2, 0xC2, 0xD7, 0xCC, 0x68, 0x9D, 0x59, 0xE0, 0x60, 0x55, 0xD0, 0x8C, 0x1C,
  0x7A, 0x57, 0x92, 0xF8, 0xB7, 0xC1, 0x1A, 0xBF, 0x86, 0xF4, 0x3B, 0x0F, 0x12, 0xCF, 0xA9, 0xC9,
  0x36, 0xA6, 0x97, 0x31, 0xC2, 0xD1, 0x44, 0xDF, 0xBB, 0xB5, 0x42, 0x0E, 0xC5, 0x42, 0x3D, 0x08,
  0xC7, 0x18, 0x1C, 0xF0, 0x3B, 0x9F, 0x5F, 0x98, 0x26, 0xAD, 0x26, 0x9A, 0x63, 0x9A, 0x58, 0xAD,
  0x6F, 0x21, 0x6B, 0xA7, 0x30, 0xB9, 0x56, 0x75, 0x0A, 0xA4, 0x28, 0x61, 0xC8, 0x19, 0x7E, 0x71,
  0xCF, 0x18, 0xAE, 0xCE, 0x7E, 0x58, 0xBD, 0x77, 0x3A, 0x1D, 0x1E, 0x69, 0x26, 0x96, 0xC7, 0x23,
  0x71, 0xE2, 0x8F, 0x86, 0xC1, 0x1A, 0x64, 0x8A, 0x45, 0xFD, 0xF2, 0x28, 0x90, 0xDA, 0x4D, 0x8F,
  0x35, 0x9A, 0x47, 0x5E, 0x4F, 0xAB, 0x3C, 0xA4, 0x7D, 0x4D, 0x74, 0xAD, 0x6F, 0x05, 0x8D, 0x9F,
  0xDA, 0x6E, 0x99, 0x2D, 0xED, 0xE2, 0x50, 0x5D, 0xE4, 0x3B, 0x55, 0x17, 0xDC, 0xD6, 0x15, 0x97,
  0x85, 0x7C, 0x21, 0x73, 0x2E, 0xAD, 0x6B, 0x75, 0xE1, 0xE1, 0x6F, 0x69, 0xA6, 0x5C, 0xAA, 0xF9,
  0xD7, 0x17, 0x72, 0xED, 0x90, 0xAA, 0x9C, 0x49, 0xF7, 0xB0, 0x31, 0x93, 0x8F, 0x63, 0x5A, 0x13,
  0xC3, 0xE0, 0x6B, 0xBB, 0x77, 0xB7, 0x9F, 0x52, 0xB6, 0x96, 0x17, 0x18, 0x68, 0xDF, 0x57, 0x94,
  0xAB, 0x0F, 0x70, 0x64, 0xAC, 0x29, 0x55, 0x54, 0xDB, 0xB1, 0xD3, 0x5A, 0x8F, 0xB5, 0x49, 0x33,
  0x90, 0x4D, 0x73, 0xC3, 0x17, 0x96, 0xBA, 0x94, 0xF7, 0xDA, 0xAD, 0xB3, 0xDC, 0xB5, 0xEC, 0x4B,
  0x1B, 0x34, 0xEE, 0x33, 0x0E, 0xE5, 0xDF, 0xC0, 0x38, 0xE9, 0xBA, 0xB4, 0xFC, 0x01, 0x79, 0x63,
  0x70, 0x93, 0x5B, 0xE9, 0xF7, 0xB1, 0xC8, 0xC6, 0xDA, 0x29, 0xA7, 0x8E, 0x29, 0x4B, 0x62, 0x5E,
  0x8C, 0x7A, 0xF1, 0xE9, 0xC7, 0xB5, 0x4A, 0xDE, 0x0D, 0xF8, 0x52, 0xC0, 0x8F, 0x27, 0x4A, 0xC7,
  0xB6, 0xA4, 0xDF, 0xFC, 0x5D, 0x3A, 0x2D, 0x07, 0xC2, 0xBA, 0x07, 0xD9, 0xE7, 0xF0, 0xAD, 0xBC,
  0x9E, 0x75, 0xDD, 0xC2, 0xDA, 0x34, 0xD6, 0x17, 0xEC, 0xE2, 0x2C, 0x82, 0xD9, 0x70, 0x4B, 0x02,
  0x38, 0x1F, 0x29, 0xEB, 0x4A, 0x33, 0x8A, 0x97, 0x31, 0x53, 0xA6, 0xE5, 0x0E, 0x43, 0xA6, 0x36,
  0xD2, 0xBC, 0x81, 0x55, 0xDB, 0x24, 0xE0, 0x72, 0x69, 0xB7, 0x16, 0x72, 0x5B, 0xAC, 0x85, 0x9B,
  0x73, 0x22, 0x33, 0x29, 0xEA, 0x38, 0x19, 0xAB, 0xA3, 0x51, 0x8A, 0xD7, 0xC3, 0xA9, 0xAC, 0x5D,
  0x1F, 0x2A, 0x3F, 0xB3, 0xAC, 0xD2, 0x6C, 0x5C, 0x91, 0x90, 0x38, 0x03, 0xEA, 0x78, 0xAE, 0x2B,
  0xC4, 0x7A, 0xF5, 0xB3, 0xEA, 0xFA, 0x0E, 0xA3, 0x15, 0xDB, 0x8B, 0x19, 0xAD, 0x66, 0x60, 0x5B,
  0x2A, 0x0E, 0xE5, 0x65, 0xC1, 0x1F, 0xEF, 0x00, 0x3F, 0x0A, 0xEF, 0xA7, 0x39, 0xCD, 0xE9, 0xB1,
  0xE5, 0x55, 0xA7, 0x0A, 0x6B, 0xDE, 0xDC, 0xCD, 0xD2, 0x34, 0x89, 0xBC, 0x4D, 0xF0, 0xF6, 0xF1,
  0x1E, 0x47, 0x92, 0xEE, 0x2B, 0x99, 0x5A, 0xD5, 0xC9, 0xF9, 0x83, 0xAE, 0x18, 0x73, 0xEF, 0x92,
  0x3F, 0x1A, 0x4F, 0x0A, 0xEB, 0xDF, 0x61, 0xF1, 0xB6, 0x91, 0xAE, 0xE7, 0x6D, 0xAF, 0x89, 0x63,
  0xFB, 0x06, 0xA0, 0xA3, 0x80, 0xB7, 0xD1, 0x7D, 0xD6, 0xC7, 0x62, 0xC0, 0x83, 0xFF, 0x00, 0x02,
  0x35, 0x9F, 0x63, 0x7F, 0x7B, 0xA5, 0x78, 0x57, 0x49, 0x7B, 0x6B, 0xC9, 0x2D, 0xC1, 0xD5, 0x64,
  0x69, 0x8A, 0x1C, 0x6E, 0x50, 0x23, 0xEB, 0xEA, 0x30, 0x4D, 0x2D, 0xAE, 0x9F, 0x0F, 0x8A, 0x2E,
  0x7C, 0x45, 0xA6, 0x69, 0xEE, 0x6C, 0xAD, 0x5A, 0xE2, 0x3D, 0x52, 0xCA, 0x69, 0x81, 0x02, 0xDE,
  0x44, 0x71, 0xB8, 0xF1, 0x9E, 0xAA, 0xCC, 0x07, 0xD0, 0x56, 0x98, 0x98, 0x5E, 0x12, 0x72, 0xD9,
  0x5F, 0xE5, 0xDF, 0xFC, 0xFE, 0x42, 0xC2, 0x57, 0x8C, 0x64, 0x95, 0xF7, 0xFF, 0x00, 0x87, 0x3D,
  0xF2, 0x99, 0x14, 0xD1, 0xCF, 0x10, 0x96, 0x19, 0x16, 0x48, 0xDB, 0xA3, 0x29, 0xC8, 0x3F, 0x43,
  0x5E, 0x63, 0xE2, 0xCF, 0x15, 0x5D, 0x5E, 0x5B, 0xB5, 0xB4, 0x32, 0x34, 0x56, 0xC4, 0x6D, 0x3B,
  0x78, 0x69, 0x7D, 0x49, 0xF4, 0x1E, 0xD5, 0xE8, 0xBA, 0x54, 0x4B, 0x06, 0x8F, 0x65, 0x12, 0x00,
  0x15, 0x20, 0x45, 0x18, 0xFF, 0x00, 0x74, 0x57, 0xCC, 0xE1, 0xB1, 0xB0, 0xC4, 0xCE, 0x4A, 0x9E,
  0xCB, 0xAF, 0x73, 0xD6, 0xA7, 0x59, 0x54, 0x9B, 0x8C, 0x7A, 0x17, 0x28, 0xA2, 0x8A, 0xED, 0x36,
  0x0A, 0x28, 0xA2, 0x80, 0x0A, 0xE0, 0x3E, 0x29, 0xC9, 0xA6, 0xC7, 0xA4, 0xDB, 0x4B, 0xAA, 0x34,
  0xB2, 0x43, 0x03, 0x97, 0x16, 0x96, 0xC5, 0x4C, 0xD3, 0x31, 0x21, 0x7E, 0x50, 0x7B, 0x00, 0x49,
  0x26, 0xBB, 0xFA, 0xF9, 0xB7, 0xE2, 0x35, 0xBF, 0x8B, 0x35, 0x0D, 0x62, 0xF3, 0xC4, 0x1A, 0x9D,
  0xA4, 0x50, 0xD8, 0x5B, 0x39, 0xB5, 0x85, 0x63, 0xBA, 0x89, 0x9A, 0x14, 0xCE, 0x42, 0x30, 0x56,
  0xCE, 0xEE, 0xE7, 0xBF, 0x3E, 0xD5, 0xB5, 0x08, 0xDE, 0x7B, 0xD8, 0xCA, 0xB3, 0xF7, 0x1E, 0x97,
  0x34, 0xED, 0xBE, 0x27, 0x6A, 0xEB, 0xAD, 0x6A, 0x8D, 0x16, 0xA3, 0x65, 0x6F, 0x63, 0x13, 0xC8,
  0xF0, 0xC6, 0x6D, 0x10, 0xFC, 0x88, 0xD8, 0x55, 0x24, 0x10, 0x49, 0x20, 0xF5, 0x27, 0xA8, 0xAD,
  0xDF, 0x83, 0x76, 0xB0, 0x5C, 0x5C, 0x6A, 0xBA, 0x8D, 0xC8, 0x94, 0xEA, 0x93, 0x20, 0x69, 0x19,
  0xD7, 0x80, 0x1D, 0xD9, 0x9B, 0x69, 0xEF, 0xCE, 0x33, 0xF8, 0x57, 0x8A, 0x5B, 0x49, 0x6A, 0xD7,
  0x2D, 0x35, 0xD3, 0xDC, 0xC4, 0x84, 0x7E, 0xF1, 0xE3, 0x87, 0x79, 0x63, 0x90, 0x79, 0xCF, 0xAE,
  0x2B, 0xD7, 0x3E, 0x10, 0x6B, 0xB1, 0xC1, 0xAA, 0x5D, 0xDB, 0x5D, 0xEA, 0x76, 0x02, 0xD2, 0x50,
  0xCF, 0x6C, 0x64, 0x9D, 0x56, 0x56, 0x67, 0x29, 0x84, 0xDB, 0x9E, 0x30, 0x17, 0xA7, 0xAF, 0xD6,
  0xBA, 0xAA, 0xD3, 0x8C, 0x61, 0x68, 0x2B, 0x1C, 0xB4, 0x64, 0xDC, 0xBD, 0xEF, 0xF8, 0x63, 0xD1,
  0xBC, 0x77, 0xA7, 0x41, 0x75, 0xE0, 0x2D, 0x72, 0x09, 0xA4, 0x58, 0xD3, 0xEC, 0xAD, 0x22, 0x33,
  0x9C, 0x00, 0xEB, 0xF3, 0x27, 0x3F, 0xEF, 0x01, 0xF9, 0xD6, 0x5F, 0x84, 0x35, 0x7F, 0xED, 0x9D,
  0x23, 0x41, 0xBB, 0x16, 0xE6, 0x05, 0x16, 0xD7, 0x31, 0xA8, 0xC8, 0x21, 0x82, 0xB4, 0x63, 0x2A,
  0x47, 0x6E, 0xDF, 0x85, 0x63, 0x7C, 0x6B, 0x3A, 0xED, 0xF6, 0x89, 0x69, 0xA5, 0x69, 0x7A, 0x4D,
  0xE5, 0xC5, 0xAB, 0xCC, 0x26, 0xB9, 0xB8, 0x81, 0x37, 0x81, 0xB7, 0xEE, 0xA6, 0xD1, 0x93, 0xD7,
  0x9E, 0x98, 0xE0, 0x55, 0x6F, 0x09, 0x1D, 0x5B, 0x47, 0xF0, 0xCF, 0x87, 0xD5, 0xAC, 0x63, 0xB1,
  0x96, 0x5B, 0xA9, 0xD5, 0x6D, 0xEE, 0xCE, 0xC3, 0xE4, 0xBB, 0xC7, 0xBB, 0x00, 0x90, 0x77, 0x1F,
  0x98, 0x81, 0xFA, 0x1A, 0xE6, 0x6B, 0xF7, 0x77, 0x6C, 0xE9, 0x8E, 0x93, 0xB2, 0x46, 0xC7, 0xC5,
  0xC6, 0x65, 0xF8, 0x67, 0xAC, 0x95, 0x62, 0x38, 0x8C, 0x71, 0xFE, 0xF8, 0xAF, 0x9E, 0x6E, 0xEC,
  0x6C, 0xAC, 0xED, 0xBC, 0x37, 0x3C, 0xE6, 0x15, 0x4B, 0x88, 0x4B, 0xDC, 0x31, 0xCB, 0x02, 0x33,
  0x9F, 0x98, 0x0E, 0x49, 0xE7, 0x19, 0xED, 0xD3, 0xB5, 0x7D, 0x0B, 0xF1, 0x7F, 0x8F, 0x86, 0x9A,
  0xCF, 0xD6, 0x3F, 0xFD, 0x18, 0x2B, 0xE7, 0x79, 0xBF, 0xB3, 0xE1, 0x1E, 0x1B, 0x93, 0x4E, 0x82,
  0x77, 0xBA, 0xF2, 0xB3, 0x72, 0x86, 0x32, 0x7C, 0xD7, 0xDE, 0x78, 0x50, 0x46, 0x1B, 0xB8, 0xE3,
  0x8C, 0x63, 0xBD, 0x2A, 0x69, 0x59, 0x91, 0x5D, 0xB5, 0x28, 0x5B, 0xBF, 0xE8, 0xF7, 0xFE, 0xB7,
  0x1D, 0x71, 0x73, 0xE1, 0xC1, 0x75, 0x1B, 0x5B, 0xC5, 0x1B, 0x42, 0x66, 0xF9, 0x95, 0x96, 0x43,
  0xB5, 0x0C, 0x60, 0x64, 0xF4, 0x24, 0x06, 0x04, 0xF1, 0xD7, 0x26, 0xBD, 0x3B, 0xF6, 0x7C, 0x63,
  0xE5, 0xF8, 0x91, 0x43, 0x7C, 0x9B, 0xE1, 0x23, 0x1C, 0x0F, 0xE3, 0xED, 0x5E, 0x5D, 0xAB, 0x4A,
  0xD2, 0x5A, 0xCD, 0x03, 0xE9, 0x77, 0x11, 0x49, 0x24, 0x89, 0x2C, 0x12, 0x34, 0x01, 0x1B, 0x01,
  0x15, 0x5F, 0x70, 0x1D, 0x41, 0xE0, 0xF1, 0xC5, 0x7A, 0x97, 0xEC, 0xF6, 0x8C, 0x89, 0xE2, 0x25,
  0x74, 0x65, 0x6C, 0xDB, 0xF0, 0xC3, 0x07, 0xA3, 0xD2, 0x9E, 0xC5, 0xD3, 0x2E, 0xF8, 0x87, 0xC4,
  0xB2, 0x13, 0x6F, 0xA6, 0xC5, 0x75, 0x37, 0xD9, 0x7F, 0xB3, 0x23, 0xB7, 0x92, 0x02, 0x70, 0xBE,
  0x60, 0x00, 0x96, 0xC7, 0x7E, 0xDC, 0xD7, 0x1F, 0x75, 0xAA, 0x09, 0xA2, 0xB7, 0x8E, 0x47, 0x2E,
  0x90, 0x46, 0x63, 0x88, 0x76, 0x50, 0xBC, 0xFE, 0xA4, 0x9A, 0xB1, 0x7B, 0x15, 0x9D, 0xD5, 0xC3,
  0xDC, 0x4A, 0x92, 0x17, 0xDA, 0xBB, 0xB7, 0x13, 0xB4, 0x0C, 0x00, 0x0F, 0x07, 0x81, 0x9A, 0x6F,
  0xF6, 0x76, 0x8B, 0x14, 0x91, 0xA5, 0xD6, 0x96, 0x0B, 0x48, 0xBB, 0xA3, 0x6F, 0xB5, 0xBA, 0xAC,
  0x83, 0xD5, 0x58, 0x92, 0xA7, 0xF3, 0x1C, 0xD7, 0x74, 0xAA, 0xD6, 0x84, 0x39, 0x69, 0x45, 0x7E,
  0xBF, 0x8D, 0x8F, 0x0E, 0xA5, 0x17, 0x56, 0x6E, 0x53, 0x91, 0x72, 0x3B, 0xFD, 0x3E, 0xE6, 0xD2,
  0x38, 0x4C, 0x9B, 0x53, 0x70, 0x3B, 0xC1, 0x04, 0x06, 0x6E, 0x31, 0xF5, 0xE2, 0xBB, 0x90, 0x96,
  0xDA, 0x1E, 0x89, 0x05, 0xAC, 0x18, 0xF3, 0x27, 0x41, 0x2C, 0xEF, 0xDD, 0x87, 0x3B, 0x07, 0xD0,
  0x72, 0x7F, 0x1A, 0xE1, 0xEC, 0xF4, 0x5F, 0x0D, 0x5C, 0x5D, 0x35, 0xBF, 0xF6, 0x55, 0xD7, 0xDA,
  0x50, 0xFC, 0xF6, 0xC9, 0x74, 0xC9, 0x38, 0xFA, 0x23, 0x64, 0x3F, 0xFC, 0x04, 0x93, 0xED, 0x5D,
  0x3D, 0xF4, 0x90, 0xBF, 0x8A, 0x6C, 0xAD, 0x51, 0x99, 0x2C, 0xA2, 0x48, 0x9F, 0xE7, 0xCE, 0x56,
  0x14, 0x84, 0x31, 0xCF, 0x7E, 0x83, 0x9A, 0xF9, 0xAC, 0xDA, 0xB6, 0x2E, 0xA5, 0x1F, 0xAB, 0xCA,
  0x4E, 0xD2, 0xB2, 0xD5, 0x7E, 0xAB, 0x7B, 0xF5, 0x2E, 0x14, 0x21, 0x4B, 0xDE, 0x8A, 0xD5, 0xE9,
  0xF7, 0x99, 0x1A, 0xC5, 0xC1, 0x37, 0x13, 0xA7, 0xFC, 0xF1, 0x02, 0x33, 0xEC, 0xD8, 0xCB, 0x7E,
  0xBC, 0x7E, 0x15, 0xEE, 0x1A, 0x04, 0xE2, 0xE7, 0xC3, 0x9A, 0x64, 0xE3, 0xF8, 0xED, 0x63, 0x6F,
  0xFC, 0x74, 0x57, 0xCD, 0xB7, 0xDA, 0x99, 0xB9, 0x56, 0x64, 0x0C, 0xB1, 0x16, 0x2D, 0xCF, 0x52,
  0xCC, 0x72, 0x49, 0xFF, 0x00, 0x68, 0xF5, 0xAF, 0x64, 0xD2, 0xFC, 0x56, 0x3C, 0x2F, 0xA7, 0xE8,
  0x7A, 0x7E, 0xB5, 0x6A, 0x61, 0xB1, 0x9E, 0xCA, 0x1F, 0x22, 0xF9, 0x4E, 0x54, 0x36, 0xC1, 0xB9,
  0x5C, 0x76, 0xC1, 0x3D, 0x79, 0xE2, 0xB9, 0xB2, 0xD8, 0x46, 0x94, 0xA5, 0xD8, 0xDF, 0x07, 0x59,
  0x2A, 0x92, 0x6F, 0x63, 0xBF, 0xA2, 0x91, 0x59, 0x5D, 0x43, 0xA3, 0x06, 0x56, 0x19, 0x04, 0x1C,
  0x82, 0x29, 0x6B, 0xDB, 0x3D, 0x80, 0xA2, 0x8A, 0xAB, 0xA9, 0xEA, 0x56, 0x9A, 0x3E, 0x99, 0x71,
  0xA8, 0xDF, 0x4C, 0x21, 0xB5, 0xB7, 0x43, 0x24, 0xAE, 0x46, 0x70, 0x07, 0xF3, 0xA0, 0x0E, 0x2B,
  0xE2, 0x8F, 0xC4, 0x19, 0x7C, 0x0F, 0xA7, 0x5A, 0x2D, 0x8C, 0x70, 0x4D, 0xA8, 0x5D, 0xB9, 0x0A,
  0x92, 0xE4, 0x84, 0x8C, 0x0E, 0x5F, 0x03, 0xDC, 0xA8, 0x15, 0xE0, 0x36, 0xD3, 0x4D, 0x3C, 0x76,
  0x69, 0xAC, 0xC8, 0xAF, 0xA7, 0xDD, 0x5D, 0xFD, 0xAA, 0x5B, 0x92, 0xC5, 0xC3, 0xC9, 0x93, 0xB8,
  0x3E, 0x0F, 0xDE, 0xC1, 0x3F, 0x9D, 0x69, 0xF8, 0x97, 0x56, 0xBB, 0xF1, 0xEF, 0x8B, 0x2E, 0x24,
  0x9E, 0x3B, 0x8F, 0xDF, 0xB6, 0xDD, 0x2D, 0x02, 0x93, 0x88, 0x87, 0x40, 0x07, 0x4E, 0x70, 0x18,
  0xFB, 0x93, 0x54, 0x66, 0xB5, 0x8F, 0x43, 0xB7, 0x5D, 0x35, 0xA5, 0x37, 0xF7, 0xB3, 0x90, 0x1A,
  0x08, 0xFE, 0x78, 0x23, 0x3E, 0xC0, 0x7D, 0xF7, 0xF7, 0xE9, 0xF5, 0xAE, 0x98, 0xD4, 0x85, 0x26,
  0xA9, 0xEF, 0x37, 0xD3, 0xFA, 0xE8, 0x70, 0xD6, 0xAA, 0x9B, 0x35, 0xAF, 0x7C, 0x16, 0xB1, 0x42,
  0x6E, 0x74, 0x09, 0xDA, 0x69, 0x50, 0xB2, 0xC9, 0x13, 0x02, 0xCB, 0x2A, 0x31, 0xCA, 0xB2, 0x11,
  0x90, 0x40, 0x1F, 0x29, 0x3D, 0x32, 0x2B, 0x97, 0x8F, 0x4F, 0xB9, 0x20, 0xCF, 0x69, 0x20, 0x79,
  0xA1, 0x62, 0x8F, 0x19, 0x41, 0x1B, 0x13, 0xEC, 0x3B, 0xFD, 0x0F, 0x3C, 0x57, 0x49, 0xA4, 0x5C,
  0xCD, 0xA7, 0xF8, 0x6E, 0x7B, 0x18, 0xEE, 0xED, 0x74, 0xDD, 0x40, 0x3E, 0xFC, 0xCD, 0x75, 0x19,
  0x12, 0x8E, 0xA5, 0x4A, 0xE7, 0xE4, 0xF4, 0xFC, 0xB3, 0x56, 0xAF, 0xED, 0x2D, 0xF5, 0xA8, 0xA1,
  0xBE, 0x4D, 0x63, 0x46, 0xB2, 0xD5, 0x22, 0xC6, 0x77, 0xDF, 0x47, 0xB6, 0x6C, 0x71, 0x97, 0xC1,
  0xEB, 0xE8, 0x7D, 0xF1, 0x5D, 0x9A, 0xAD, 0xCC, 0x53, 0x4D, 0xE8, 0x7A, 0x2F, 0x82, 0x7E, 0x27,
  0x3E, 0xBE, 0x97, 0x2F, 0xAB, 0xAC, 0x16, 0x28, 0xAD, 0x88, 0x8A, 0xBF, 0x08, 0xA1, 0x79, 0x2E,
  0x49, 0xCF, 0x24, 0x1E, 0x70, 0x2A, 0xCE, 0xB7, 0xAF, 0xE9, 0xDA, 0xFE, 0xA5, 0xA5, 0xC5, 0x61,
  0x20, 0xBB, 0x11, 0x4C, 0x1D, 0x99, 0xEC, 0x9A, 0x54, 0x20, 0xC9, 0x1E, 0x08, 0xCE, 0x0F, 0x6C,
  0xEF, 0x19, 0xDB, 0xC1, 0xE9, 0x9A, 0xF2, 0x48, 0xF4, 0x7B, 0x9B, 0x69, 0x5A, 0x4B, 0x4D, 0x7F,
  0xC3, 0xB6, 0xE6, 0x4E, 0x64, 0xD9, 0xA8, 0xC4, 0x73, 0xCE, 0x48, 0x19, 0x3C, 0x03, 0xE9, 0x5D,
  0x36, 0x88, 0x34, 0x5B, 0x4D, 0x66, 0x3B, 0xAD, 0x57, 0x51, 0xD2, 0x3C, 0xA2, 0x85, 0x82, 0x47,
  0x7E, 0x92, 0x08, 0xE5, 0x56, 0x46, 0x56, 0x05, 0x4E, 0x47, 0x4F, 0xD2, 0xB9, 0xE7, 0x4E, 0x1C,
  0xAD, 0xA3, 0x68, 0x4E, 0x5C, 0xEA, 0xE7, 0x71, 0xF1, 0x87, 0xFE, 0x49, 0x9E, 0xB5, 0xF5, 0x8F,
  0xFF, 0x00, 0x46, 0x0A, 0xF9, 0xE6, 0xF6, 0xF3, 0x52, 0x58, 0x74, 0x16, 0xB9, 0xD2, 0xD6, 0x34,
  0x00, 0x35, 0xB0, 0x2B, 0x91, 0x3F, 0x40, 0x3A, 0x72, 0x3B, 0x1C, 0x67, 0xB8, 0xF6, 0xAF, 0x79,
  0xF1, 0xAE, 0xB7, 0xE1, 0xAF, 0x15, 0x78, 0x63, 0x50, 0xD1, 0xE3, 0xF1, 0x46, 0x97, 0x0B, 0xDC,
  0x01, 0xB2, 0x46, 0x9C, 0x15, 0x04, 0x30, 0x23, 0x38, 0xED, 0xC5, 0x79, 0xA5, 0xF7, 0x86, 0xBF,
  0xB5, 0x2D, 0x34, 0xBB, 0x7B, 0xAF, 0x88, 0x3E, 0x1B, 0x55, 0xD3, 0x53, 0x65, 0xB9, 0x8C, 0xB6,
  0x7B, 0x72, 0x7D, 0x4F, 0x03, 0xF2, 0xAE, 0x27, 0xED, 0x53, 0x5C, 0x8B, 0x4E, 0xA7, 0x4C, 0xF5,
  0x68, 0xE7, 0x22, 0xF1, 0x26, 0xB5, 0x14, 0xE2, 0x65, 0xB3, 0x2F, 0x22, 0xAB, 0x44, 0x1B, 0xEC,
  0xEF, 0x85, 0xC9, 0x04, 0xE4, 0x96, 0xF5, 0x5C, 0xFE, 0x26, 0xBD, 0x33, 0xE0, 0x36, 0xA3, 0x71,
  0xAA, 0x5C, 0x78, 0x92, 0xF2, 0xEE, 0x4F, 0x32, 0x67, 0x36, 0xE0, 0xB6, 0x31, 0x9C, 0x07, 0xAE,
  0x6D, 0x3C, 0x25, 0x03, 0x31, 0x79, 0xBE, 0x28, 0x69, 0x00, 0x91, 0xB4, 0xEC, 0x00, 0xF1, 0xF9,
  0x8F, 0x5A, 0xF4, 0x2F, 0x87, 0xBE, 0x11, 0xB3, 0xF0, 0x46, 0x9D, 0x78, 0xF0, 0x6A, 0xCB, 0xA8,
  0x0B, 0xE6, 0x47, 0x13, 0x22, 0x85, 0x4D, 0xAA, 0x0E, 0x31, 0x82, 0x73, 0xD4, 0xF3, 0x42, 0x75,
  0x5A, 0xF7, 0xD0, 0xA9, 0xC1, 0x27, 0x73, 0xCB, 0x6E, 0xEF, 0x78, 0xF2, 0x11, 0x4E, 0x41, 0x01,
  0x9B, 0xBB, 0x7A, 0x00, 0x07, 0x6F, 0xE6, 0x6B, 0x4F, 0xC4, 0x96, 0x5A, 0x86, 0x87, 0xA1, 0x69,
  0x3A, 0x5E, 0xA3, 0x25, 0xB8, 0x91, 0xF7, 0xDE, 0x2D, 0xB0, 0x53, 0xE7, 0x5B, 0x87, 0xC0, 0xC3,
  0x9E, 0x98, 0x6C, 0x67, 0x1E, 0xA0, 0xD4, 0x1E, 0x1E, 0xD5, 0x86, 0x81, 0xA9, 0x2E, 0xB1, 0x2E,
  0x9F, 0x61, 0x76, 0xF9, 0x26, 0xD8, 0x5E, 0x5D, 0x08, 0xF6, 0x90, 0x71, 0xBD, 0x57, 0x3C, 0xFD,
  0x7F, 0x2E, 0x6B, 0x27, 0x53, 0xBC, 0xBC, 0xD6, 0xF5, 0x39, 0xEF, 0xEF, 0x6F, 0xAD, 0x25, 0xBA,
  0x9D, 0xB7, 0x3B, 0x9B, 0x94, 0x03, 0xD8, 0x0E, 0x78, 0x00, 0x70, 0x05, 0x7B, 0x16, 0x93, 0x6B,
  0xB1, 0xE5, 0xDE, 0x29, 0x3D, 0x75, 0x67, 0x45, 0x6F, 0x6C, 0x75, 0xAF, 0x86, 0xB7, 0xF7, 0xD2,
  0x67, 0xED, 0x7A, 0x0D, 0xC4, 0x62, 0x19, 0xFF, 0x00, 0x88, 0xC2, 0xE7, 0xFD, 0x59, 0x3D, 0xF6,
  0xB1, 0xDC, 0x3D, 0x3A, 0x55, 0xF4, 0xD4, 0xA7, 0xD4, 0xFC, 0x3F, 0x37, 0x88, 0x6E, 0x65, 0x69,
  0x67, 0xFE, 0xCA, 0x9A, 0xCE, 0x79, 0x18, 0xF2, 0x65, 0x0C, 0x91, 0x82, 0x7D, 0xCA, 0x48, 0xBF,
  0x88, 0x35, 0x56, 0x1D, 0x52, 0xDE, 0xD3, 0xE1, 0xAD, 0xCF, 0x86, 0xED, 0x64, 0xD3, 0xC5, 0xE5,
  0xF4, 0xCB, 0x2D, 0xCD, 0xC3, 0xEA, 0x30, 0xE3, 0x01, 0x81, 0xC6, 0x33, 0xE8, 0xA0, 0x7E, 0x75,
  0x63, 0x47, 0x87, 0x49, 0x8F, 0xC1, 0xB6, 0xFE, 0x1D, 0xBD, 0xF1, 0x06, 0x97, 0x0B, 0x5E, 0x5F,
  0x35, 0xD5, 0xEC, 0x89, 0x79, 0x1B, 0x79, 0x30, 0xA0, 0x18, 0x40, 0x73, 0x82, 0xCC, 0xC1, 0x71,
  0xE9, 0xDF, 0xA5, 0x70, 0x63, 0x29, 0xF3, 0xD3, 0x7A, 0x6A, 0x9E, 0x86, 0xDC, 0xAA, 0x49, 0x5B,
  0xB7, 0xE2, 0x62, 0xF8, 0x6B, 0x4A, 0x9F, 0xC5, 0x3A, 0xED, 0xBE, 0x9E, 0x8A, 0x52, 0xCE, 0x33,
  0xBE, 0x76, 0x1F, 0x76, 0x28, 0x87, 0xDE, 0x24, 0xFA, 0x9E, 0x9E, 0xE4, 0xD7, 0xB1, 0x78, 0xB8,
  0xDB, 0x6B, 0x7E, 0x12, 0xBE, 0x8C, 0x2A, 0xE2, 0xD2, 0x48, 0x9A, 0x1E, 0x3E, 0xEF, 0x3B, 0x71,
  0xF9, 0x13, 0x5C, 0x77, 0xF6, 0xD6, 0x97, 0xA6, 0x59, 0xB5, 0x86, 0x8F, 0x7D, 0xA2, 0x59, 0x59,
  0x9E, 0x48, 0x8E, 0xFD, 0x19, 0xE4, 0x3F, 0xDE, 0x77, 0xCE, 0x58, 0xFE, 0x83, 0xB0, 0xAD, 0x44,
  0x98, 0x5B, 0xE8, 0x70, 0xD9, 0xCB, 0x2A, 0xB3, 0x5D, 0x1F, 0xB6, 0xCC, 0xC1, 0xB2, 0x16, 0x05,
  0x18, 0x43, 0x9F, 0x46, 0x39, 0x23, 0xD9, 0x6B, 0x92, 0xBE, 0x0A, 0x9E, 0x0B, 0x2E, 0xA9, 0x52,
  0x52, 0x4E, 0x56, 0xE8, 0x79, 0xF1, 0xAB, 0x2E, 0x7F, 0x64, 0xA0, 0xF9, 0x5D, 0xEE, 0xDA, 0xB5,
  0xFB, 0x1B, 0x5F, 0x08, 0xF5, 0xA9, 0xAF, 0xB4, 0x4B, 0xFD, 0x26, 0xE1, 0xCB, 0xBE, 0x95, 0x71,
  0xE5, 0x23, 0x13, 0xCF, 0x96, 0x72, 0x54, 0x7E, 0x18, 0x23, 0xE9, 0x8A, 0xF4, 0x4A, 0xF2, 0x3F,
  0x81, 0x88, 0xF7, 0x36, 0xFE, 0x21, 0xD6, 0x0A, 0x91, 0x15, 0xDD, 0xE0, 0x58, 0xC9, 0xEF, 0x8C,
  0xB7, 0xF2, 0x71, 0x5E, 0xB9, 0x44, 0x22, 0xE3, 0x14, 0xA5, 0xB9, 0xEF, 0xD0, 0xBA, 0xA6, 0x93,
  0x0A, 0xF2, 0x7F, 0x8B, 0x9A, 0x6F, 0x89, 0x3C, 0x45, 0x25, 0xBE, 0x99, 0xA5, 0x69, 0x77, 0x92,
  0x59, 0xC6, 0xB9, 0x79, 0x63, 0x61, 0xB2, 0x56, 0x6C, 0x76, 0xCF, 0x6C, 0x77, 0xF5, 0xAF, 0x58,
  0xAE, 0x47, 0xC7, 0xFE, 0x22, 0x93, 0x46, 0xD2, 0x61, 0xB2, 0xB3, 0x90, 0xA6, 0xA3, 0xA9, 0x39,
  0x86, 0x16, 0x1D, 0x63, 0x50, 0x32, 0xF2, 0x7E, 0x03, 0xA7, 0xB9, 0x15, 0x5E, 0xD5, 0x52, 0x4E,
  0x6F, 0xA0, 0xEB, 0x5B, 0x91, 0xDD, 0x9E, 0x35, 0x7A, 0xD0, 0xF8, 0x37, 0x41, 0x96, 0xC6, 0x29,
  0xC4, 0xF7, 0x11, 0x8F, 0x2E, 0xEE, 0xE1, 0x5B, 0xEF, 0x48, 0x7A, 0xC1, 0x11, 0xEC, 0xA3, 0xF8,
  0x88, 0xEA, 0x41, 0xEC, 0x2B, 0x1F, 0xC0, 0xFE, 0x16, 0xBE, 0xF1, 0xAE, 0xBA, 0x6C, 0xD2, 0xE1,
  0xAD, 0xD5, 0xE3, 0xF3, 0xEF, 0xEE, 0xD4, 0x7C, 0xD1, 0xC4, 0x4E, 0x16, 0x35, 0xF7, 0x6F, 0xE5,
  0xF4, 0xC1, 0xCA, 0xF1, 0xB5, 0xD2, 0x9D, 0x4E, 0x3D, 0x36, 0x11, 0xB6, 0x0B, 0x34, 0x0B, 0x81,
  0xFD, 0xF2, 0x32, 0x7F, 0x4C, 0x0F, 0xCE, 0xBD, 0x77, 0xE0, 0x13, 0xDB, 0x1B, 0x3F, 0x10, 0x05,
  0x65, 0xFB, 0x57, 0xDA, 0x63, 0x2C, 0xBD, 0xFC, 0xBD, 0x9F, 0x2F, 0xE1, 0x9D, 0xD4, 0x60, 0x69,
  0xB8, 0x51, 0x96, 0x25, 0xAF, 0x7A, 0x7A, 0xFA, 0x27, 0xB7, 0xE0, 0x70, 0xD1, 0xA6, 0xA7, 0x34,
  0xE5, 0xFD, 0x79, 0x1D, 0x35, 0xB7, 0xC1, 0xBF, 0x02, 0x5A, 0xDB, 0xAC, 0x4F, 0xA4, 0x19, 0x88,
  0x1F, 0xEB, 0x26, 0xB8, 0x93, 0x73, 0x7D, 0x70, 0xC0, 0x7E, 0x42, 0xA4, 0xFF, 0x00, 0x85, 0x47,
  0xE0, 0x2F, 0xFA, 0x01, 0x45, 0xFF, 0x00, 0x81, 0x12, 0xFF, 0x00, 0xF1, 0x55, 0x47, 0xC7, 0x1F,
  0x0A, 0x0F, 0x8D, 0x35, 0xF1, 0xAA, 0x3F, 0x88, 0x6E, 0x6D, 0x14, 0x42, 0xB1, 0x2C, 0x02, 0x1D,
  0xEA, 0xB8, 0xCF, 0x23, 0xE6, 0x1D, 0x73, 0x5C, 0xB4, 0xFF, 0x00, 0xB3, 0xD3, 0x88, 0x58, 0xDB,
  0xF8, 0xAA, 0x53, 0x2E, 0x3E, 0x51, 0x25, 0xB6, 0x14, 0x9F, 0x72, 0x1E, 0xB6, 0x4E, 0xEB, 0x59,
  0x1D, 0xCD, 0x7F, 0x74, 0xE7, 0xFC, 0x5D, 0xA1, 0x78, 0x4D, 0xFE, 0x24, 0x69, 0x3E, 0x15, 0xF0,
  0xFE, 0x99, 0x1C, 0x49, 0x1C, 0xC3, 0xFB, 0x42, 0x65, 0x95, 0xDB, 0x77, 0xF1, 0x18, 0xC6, 0x58,
  0xE3, 0x0A, 0x0E, 0x71, 0xDC, 0xFB, 0x57, 0x68, 0x7E, 0x1B, 0xF8, 0x3B, 0xB6, 0x8B, 0x18, 0xFF,
  0x00, 0xB6, 0xF2, 0x7F, 0xF1, 0x55, 0xC0, 0x7C, 0x37, 0xD0, 0x2E, 0x74, 0xEF, 0x88, 0x7A, 0x8C,
  0x37, 0xC8, 0x04, 0xFA, 0x54, 0x72, 0x47, 0x26, 0x0E, 0x46, 0xF2, 0x76, 0x82, 0x0F, 0x7C, 0x8C,
  0x9A, 0xEF, 0xFC, 0x61, 0xE1, 0x73, 0xE2, 0xDB, 0x4B, 0x5B, 0x63, 0xA8, 0xBD, 0x9C, 0x70, 0x48,
  0x64, 0x3B, 0x23, 0xDD, 0xBC, 0x91, 0x81, 0xDC, 0x74, 0xE7, 0xF3, 0xAF, 0x97, 0xCE, 0xF1, 0xD2,
  0x8E, 0x36, 0x38, 0x75, 0x59, 0xC2, 0x29, 0x6A, 0xD5, 0xF7, 0x77, 0x7B, 0x2D, 0xFA, 0x1C, 0xF2,
  0x7A, 0x91, 0x37, 0xC3, 0x8F, 0x07, 0x8F, 0xF9, 0x84, 0x47, 0xFF, 0x00, 0x7F, 0xDF, 0xFF, 0x00,
  0x8A, 0xAE, 0x23, 0xC7, 0x3A, 0x07, 0x86, 0xF4, 0xE9, 0xB4, 0xFD, 0x17, 0x47, 0xD3, 0x63, 0x5D,
  0x56, 0xFE, 0x64, 0x50, 0xC2, 0x47, 0x6F, 0x29, 0x0B, 0x05, 0xCE, 0x09, 0xC6, 0x49, 0xE3, 0xE8,
  0x0D, 0x4F, 0x77, 0xF0, 0x9E, 0xDA, 0xCA, 0xD6, 0x5B, 0x9B, 0x8F, 0x12, 0xCC, 0x90, 0xC2, 0x85,
  0xE4, 0x73, 0x07, 0x01, 0x47, 0x5F, 0xE2, 0xAE, 0x7F, 0xE1, 0x66, 0x9A, 0xBA, 0xA7, 0xC4, 0xFD,
  0x25, 0x01, 0x67, 0x8A, 0x09, 0x5A, 0xE7, 0x73, 0x8E, 0x76, 0xA0, 0x25, 0x49, 0xFC, 0x76, 0xD6,
  0xF9, 0x4D, 0x0F, 0x6D, 0x57, 0xDB, 0x47, 0x15, 0x2A, 0x91, 0x86, 0xEA, 0xD2, 0x4A, 0xFD, 0x37,
  0x7A, 0xFA, 0x0A, 0x3A, 0xBB, 0x1E, 0xEF, 0xFF, 0x00, 0x0A, 0x77, 0xC0, 0xBE, 0x5E, 0xC3, 0xA1,
  0xA9, 0x3B, 0x71, 0xBB, 0xCF, 0x93, 0x3F, 0x5F, 0xBD, 0x5F, 0x3C, 0x1D, 0x43, 0x58, 0xF0, 0xB5,
  0xDE, 0xAD, 0xE1, 0xAF, 0xB6, 0xC9, 0x0D, 0xB2, 0x4C, 0xF0, 0xCE, 0x07, 0x55, 0x0A, 0x48, 0x2C,
  0x9E, 0x85, 0x87, 0xA7, 0x5C, 0xD7, 0xD6, 0x6F, 0xAA, 0xDA, 0xC7, 0xAE, 0x43, 0xA4, 0x33, 0x62,
  0xEA, 0x6B, 0x77, 0xB9, 0x41, 0xEA, 0x8A, 0xCA, 0xA7, 0xF5, 0x61, 0x5E, 0x61, 0xAF, 0xFC, 0x2E,
  0xFE, 0xDE, 0xF8, 0xC4, 0x9A, 0x94, 0xF0, 0xFF, 0x00, 0xC4, 0x96, 0x48, 0x12, 0xEA, 0xEB, 0x8E,
  0x24, 0x95, 0x7E, 0x5F, 0x2F, 0xF1, 0xDA, 0xA4, 0xFB, 0x67, 0xD6, 0xBE, 0x8E, 0x95, 0x4B, 0x3F,
  0x7C, 0xE8, 0xA9, 0x0B, 0xAF, 0x74, 0xAB, 0xF0, 0xF7, 0xE1, 0x76, 0x9B, 0xAC, 0xE8, 0x5F, 0xDB,
  0x7E, 0x29, 0xD3, 0xFC, 0xD9, 0x6F, 0x42, 0x9B, 0x4B, 0x52, 0xEC, 0x82, 0xDE, 0x00, 0x30, 0x83,
  0xE5, 0x23, 0x92, 0x39, 0xE7, 0xB6, 0x3B, 0x93, 0x58, 0xFF, 0x00, 0x10, 0x3E, 0x1D, 0xE9, 0x07,
  0xC6, 0x1E, 0x1D, 0xF0, 0xCF, 0x86, 0x2C, 0x12, 0xCA, 0xE6, 0xF5, 0x64, 0x96, 0xE2, 0x5D, 0xEE,
  0xFB, 0x63, 0x04, 0x7C, 0xC4, 0x31, 0x3D, 0x00, 0x6F, 0xA9, 0xC0, 0xAF, 0x73, 0xB5, 0xD4, 0x6D,
  0x6E, 0x6F, 0xEE, 0xEC, 0x6D, 0xDB, 0x73, 0xD9, 0x6C, 0x59, 0xB6, 0x8E, 0x11, 0x98, 0x64, 0x2F,
  0xD7, 0x18, 0x3F, 0x88, 0xAF, 0x3E, 0xD3, 0x35, 0x1B, 0x7B, 0xEF, 0xDA, 0x13, 0x57, 0x8A, 0x47,
  0x53, 0x25, 0x9E, 0x92, 0xB6, 0xF0, 0x02, 0x7B, 0xEE, 0x57, 0x7C, 0x7B, 0xFC, 0xDF, 0xCE, 0x92,
  0xA9, 0x27, 0x27, 0x26, 0x1C, 0x91, 0x51, 0x51, 0x45, 0xCB, 0x0F, 0x82, 0xFE, 0x08, 0xB2, 0xB3,
  0x48, 0xA7, 0xD3, 0x9E, 0xF2, 0x40, 0x3E, 0x69, 0xA7, 0x9D, 0xC3, 0x31, 0xFA, 0x29, 0x00, 0x7E,
  0x02, 0xAC, 0x7F, 0xC2, 0xA2, 0xF0, 0x17, 0xFD, 0x00, 0xA2, 0xFF, 0x00, 0xC0, 0x89, 0x7F, 0xF8,
  0xAA, 0x4F, 0x1F, 0xFC, 0x38, 0x3E, 0x3B, 0xB9, 0xB1, 0x95, 0xB5, 0xBB, 0x8B, 0x14, 0xB5, 0x46,
  0x51, 0x12, 0x47, 0xBD, 0x58, 0xB1, 0x1F, 0x37, 0xDE, 0x18, 0x3C, 0x62, 0xBC, 0xD7, 0xC4, 0x5F,
  0x05, 0xA2, 0xD0, 0x2D, 0x61, 0xBA, 0x93, 0xC5, 0x8D, 0xE4, 0xB4, 0x81, 0x19, 0x1E, 0xDC, 0x87,
  0x60, 0x7F, 0xB8, 0x03, 0x1C, 0x9F, 0xAE, 0x07, 0xBD, 0x4B, 0x9A, 0x51, 0xE6, 0x9C, 0xEC, 0x13,
  0x6A, 0x0A, 0xED, 0x68, 0x33, 0xE2, 0x47, 0x86, 0x3C, 0x33, 0x61, 0xAA, 0xDB, 0xE8, 0x7E, 0x1A,
  0xD0, 0x19, 0xEE, 0x61, 0x0B, 0x75, 0xA8, 0xC9, 0x04, 0x8E, 0xED, 0x14, 0x44, 0xE1, 0x53, 0x96,
  0x23, 0x2C, 0x48, 0xF7, 0xE9, 0x8E, 0xB5, 0x5B, 0xC6, 0x17, 0x97, 0xB6, 0x16, 0x29, 0xA2, 0x28,
  0x12, 0xF8, 0x8B, 0x58, 0x64, 0x59, 0xA1, 0x83, 0x91, 0x04, 0x7C, 0x2A, 0x42, 0xBE, 0xD8, 0xC2,
  0x8F, 0xF8, 0x11, 0xAD, 0x84, 0xD4, 0xAD, 0xFC, 0x13, 0xA1, 0x5D, 0x4B, 0xA3, 0xDA, 0xA4, 0x66,
  0x1B, 0x76, 0x74, 0x12, 0x36, 0xE7, 0x92, 0x42, 0x55, 0x37, 0xC8, 0xDD, 0x58, 0xFC, 0xDE, 0xC0,
  0x74, 0x18, 0xAD, 0xBF, 0x85, 0x3F, 0x0F, 0xEF, 0x21, 0xBC, 0x6F, 0x18, 0xF8, 0x99, 0x59, 0xF5,
  0x5B, 0x9C, 0xBD, 0xBC, 0x52, 0x8F, 0x9A, 0x20, 0xDF, 0xC6, 0xC3, 0xB3, 0x11, 0xC0, 0x1F, 0xC2,
  0x3F, 0x4E, 0x4A, 0x75, 0xA3, 0x8D, 0x82, 0x95, 0xBD, 0xC8, 0xBE, 0xBF, 0x69, 0xAD, 0xB4, 0xEC,
  0x9E, 0xBF, 0x71, 0xCD, 0x1B, 0x57, 0x97, 0x32, 0xD9, 0x1D, 0xF7, 0x82, 0xFC, 0x36, 0x9E, 0x13,
  0xF0, 0x95, 0x86, 0x90, 0xA4, 0x34, 0xB1, 0x26, 0xE9, 0x9C, 0x7F, 0x14, 0x8D, 0xCB, 0x1F, 0xA6,
  0x4E, 0x07, 0xB0, 0x15, 0xBF, 0x45, 0x15, 0xBB, 0x77, 0x77, 0x3B, 0x92, 0xB0, 0x57, 0x8A, 0xF8,
  0xC2, 0xF5, 0xB5, 0x1F, 0x8B, 0xB7, 0x36, 0xCC, 0x72, 0x9A, 0x6E, 0x9C, 0x89, 0x1A, 0xFA, 0x17,
  0x21, 0x98, 0xFE, 0x44, 0x0F, 0xC2, 0xBD, 0xAA, 0xBC, 0x17, 0xE2, 0x40, 0x6F, 0x0C, 0x7C, 0x5D,
  0xB6, 0xD6, 0xAE, 0x01, 0x1A, 0x7E, 0xA9, 0x6C, 0xB1, 0xBC, 0x98, 0xE1, 0x4A, 0x80, 0xA7, 0xF2,
  0xC2, 0x1F, 0xA1, 0xAE, 0x6C, 0x5D, 0x39, 0x55, 0xC3, 0x54, 0x84, 0x37, 0x69, 0xD8, 0xC7, 0x10,
  0x9B, 0xA6, 0xD2, 0x3C, 0xCB, 0xC5, 0xD6, 0x92, 0x41, 0xE2, 0xDD, 0x4A, 0x37, 0x1C, 0x9B, 0x86,
  0x20, 0x9F, 0x43, 0xC8, 0x3F, 0x91, 0x15, 0xD1, 0x78, 0x3F, 0xC0, 0xDE, 0x38, 0xD4, 0x6D, 0xA2,
  0xF1, 0x07, 0x86, 0xA4, 0xFB, 0x1A, 0x36, 0xE8, 0xE3, 0x98, 0xDC, 0xF9, 0x4D, 0x22, 0x83, 0x82,
  0x71, 0x8E, 0x57, 0x23, 0x1C, 0xFA, 0x57, 0x59, 0xE2, 0xEF, 0x06, 0x49, 0xE2, 0x5B, 0x38, 0x6F,
  0x74, 0xC5, 0x8E, 0x4B, 0xE8, 0xD5, 0x56, 0x48, 0xBC, 0xC0, 0x9F, 0x6A, 0x88, 0x74, 0xD8, 0xC7,
  0x80, 0xF8, 0xE3, 0x9E, 0xA3, 0x1E, 0x9C, 0xEB, 0x5A, 0xFC, 0x45, 0xF1, 0x66, 0x95, 0x69, 0x0E,
  0x9F, 0x6D, 0xF0, 0xD2, 0xEA, 0x08, 0x2D, 0x90, 0x45, 0x1C, 0x41, 0xDF, 0x0A, 0xA0, 0x60, 0x0F,
  0xB9, 0x5D, 0x78, 0x3C, 0x6C, 0x31, 0x38, 0x68, 0x4A, 0x8F, 0x65, 0x75, 0xD9, 0xF6, 0x30, 0xA3,
  0x14, 0xF5, 0x91, 0x47, 0xFE, 0x11, 0xEF, 0x8D, 0xB8, 0xFF, 0x00, 0x90, 0xE4, 0x7F, 0xF8, 0x11,
  0x1F, 0xFF, 0x00, 0x11, 0x5E, 0xA1, 0xE0, 0xDB, 0x5D, 0x7B, 0x4F, 0xF0, 0xBC, 0x31, 0xF8, 0xA6,
  0xF9, 0x2E, 0x75, 0x20, 0xCE, 0xF2, 0xC8, 0x08, 0xC2, 0xAE, 0x78, 0x19, 0x00, 0x03, 0x81, 0xDE,
  0xB8, 0x3F, 0xF8, 0x5A, 0xBE, 0x33, 0xFF, 0x00, 0xA2, 0x75, 0x77, 0xFF, 0x00, 0x7F, 0x1F, 0xFF,
  0x00, 0x88, 0xAE, 0x73, 0xC5, 0x9E, 0x34, 0xF8, 0x8B, 0xE2, 0x6D, 0x32, 0x5D, 0x36, 0xDF, 0xC2,
  0xD7, 0x7A, 0x6D, 0xAC, 0xCB, 0xB6, 0x6F, 0x2A, 0x27, 0x79, 0x24, 0x53, 0xD5, 0x77, 0x10, 0x30,
  0x0F, 0x7C, 0x0F, 0xC6, 0xB5, 0x71, 0x94, 0xB4, 0x76, 0x47, 0x47, 0x34, 0x56, 0xA6, 0x8F, 0x84,
  0xEE, 0x63, 0xD5, 0x35, 0x4F, 0x12, 0xF8, 0x82, 0x3E, 0x63, 0xD4, 0x35, 0x27, 0x10, 0xB7, 0xAC,
  0x69, 0xC2, 0x9F, 0xD6, 0xA9, 0x78, 0x92, 0xDF, 0xC7, 0x37, 0x3A, 0xD4, 0x8F, 0xA1, 0xDF, 0x25,
  0xBD, 0x80, 0x45, 0x54, 0x4F, 0x35, 0x41, 0x27, 0x1C, 0x92, 0x08, 0x3D, 0xEB, 0x23, 0x41, 0xD4,
  0xBC, 0x53, 0xE1, 0xFD, 0x16, 0xDF, 0x4D, 0x8F, 0xC1, 0x57, 0xB2, 0x2C, 0x59, 0x26, 0x42, 0x1C,
  0x16, 0x24, 0x92, 0x4E, 0x36, 0xFB, 0xD6, 0x8F, 0xFC, 0x25, 0x7E, 0x2C, 0xCF, 0xFC, 0x88, 0xD7,
  0x9F, 0x9B, 0xFF, 0x00, 0xF1, 0x35, 0xF2, 0x38, 0x8C, 0xBB, 0x31, 0xFA, 0xF5, 0x4C, 0x4C, 0x29,
  0x46, 0x49, 0xE8, 0xB9, 0xB9, 0x5A, 0xB6, 0xCB, 0x4B, 0xF6, 0x47, 0x2B, 0xE6, 0xBD, 0xCC, 0x5B,
  0xFF, 0x00, 0x0F, 0x7C, 0x44, 0xD5, 0x2C, 0xDE, 0xD2, 0xFB, 0x51, 0x8A, 0x7B, 0x77, 0xC6, 0xE8,
  0xCD, 0xC2, 0x80, 0xD8, 0x39, 0x19, 0xC0, 0xF5, 0xAE, 0x8F, 0xE0, 0x57, 0x87, 0xE5, 0xB3, 0xF1,
  0x6E, 0xBD, 0x3D, 0xC7, 0x96, 0xF2, 0x58, 0xC2, 0xB6, 0xA5, 0xA3, 0x6D, 0xCA, 0x1D, 0xDB, 0x24,
  0x03, 0xED, 0xB3, 0x15, 0x46, 0xE3, 0xC4, 0xBE, 0x32, 0x9A, 0xD6, 0x68, 0xED, 0xFC, 0x17, 0x7B,
  0x0C, 0xAE, 0x85, 0x52, 0x5D, 0xAE, 0xDB, 0x09, 0xEF, 0x8D, 0xBC, 0xE2, 0xA2, 0xF0, 0x27, 0x88,
  0xBC, 0x4F, 0xE0, 0x3D, 0x32, 0xEA, 0xC9, 0x3C, 0x17, 0x79, 0x79, 0x2D, 0xCC, 0xFE, 0x73, 0xCD,
  0x21, 0x64, 0x27, 0x80, 0x00, 0xC6, 0xD3, 0xE8, 0x4F, 0xE3, 0x5E, 0xDE, 0x59, 0x4F, 0x1C, 0xA1,
  0x38, 0xD7, 0x84, 0x60, 0xBA, 0x28, 0xA4, 0xBD, 0x5E, 0x8D, 0x97, 0x4D, 0xD9, 0xDE, 0x5A, 0x1B,
  0x5E, 0x2E, 0xF1, 0x67, 0xF6, 0x67, 0xED, 0x07, 0xA4, 0xB9, 0x90, 0x0B, 0x7B, 0x38, 0xE2, 0xB3,
  0x97, 0x9E, 0x00, 0x97, 0x25, 0x89, 0xFA, 0x6F, 0x53, 0xF8, 0x57, 0xA9, 0x78, 0xD7, 0xC5, 0x36,
  0xFE, 0x0E, 0xF0, 0xC5, 0xD6, 0xAD, 0x3E, 0x1A, 0x45, 0x1B, 0x2D, 0xE2, 0x27, 0xFD, 0x6C, 0xA7,
  0xEE, 0xAF, 0xD3, 0xB9, 0xF6, 0x06, 0xBE, 0x69, 0xF1, 0x16, 0x8B, 0xE2, 0xDF, 0x11, 0x78, 0x8A,
  0xFF, 0x00, 0x59, 0x97, 0xC3, 0x9A, 0x8C, 0x52, 0x5D, 0xCA, 0x64, 0xD8, 0xB0, 0xB1, 0xD9, 0xD0,
  0x00, 0x0E, 0x39, 0xC0, 0x02, 0xB5, 0xFC, 0x6B, 0x71, 0xE3, 0xAF, 0x1A, 0xB5, 0x87, 0xDB, 0x7C,
  0x3B, 0xA8, 0x43, 0x0D, 0x9C, 0x21, 0x16, 0x35, 0x89, 0x88, 0x77, 0xFE, 0x29, 0x0F, 0x1D, 0x4E,
  0x07, 0x1D, 0xAB, 0xD5, 0x74, 0x93, 0xE5, 0x35, 0x55, 0x2D, 0x73, 0xD7, 0xFE, 0x0D, 0xC7, 0x73,
  0x27, 0x81, 0x4E, 0xA9, 0x7A, 0xE6, 0x4B, 0xBD, 0x52, 0xF2, 0x6B, 0xC9, 0x64, 0x6E, 0xAC, 0x4B,
  0x6D, 0xCF, 0xFE, 0x3B, 0x5E, 0x31, 0x67, 0xA1, 0xF8, 0x9B, 0xC7, 0x5E, 0x39, 0xD7, 0x35, 0xAF,
  0x0D, 0x9F, 0x2E, 0x58, 0x6F, 0x5E, 0x51, 0x73, 0xE7, 0x18, 0xB6, 0x65, 0x88, 0x50, 0xAD, 0xEB,
  0xB4, 0x7E, 0x55, 0xD7, 0xE9, 0x5E, 0x3E, 0xF1, 0x5E, 0x87, 0xE1, 0x5B, 0x6D, 0x0A, 0xDF, 0xC0,
  0xB7, 0x4A, 0x2D, 0xED, 0x7C, 0x84, 0x98, 0xB3, 0x03, 0x9C, 0x63, 0x7E, 0x36, 0xFA, 0xF3, 0x8C,
  0xD5, 0x3F, 0x07, 0xF8, 0xE3, 0x5C, 0xF0, 0x5F, 0x87, 0x61, 0xD2, 0x2D, 0x3C, 0x0B, 0x3C, 0x9B,
  0x49, 0x79, 0xA7, 0x69, 0x18, 0x34, 0xAE, 0x7A, 0xB1, 0x1B, 0x38, 0xEC, 0x31, 0xE8, 0x05, 0x38,
  0xC2, 0x69, 0xB6, 0x96, 0xE2, 0x95, 0x48, 0x68, 0x9B, 0x34, 0x87, 0x87, 0x7E, 0x36, 0x05, 0x0A,
  0x35, 0xD8, 0xFD, 0x32, 0x6E, 0x50, 0xFF, 0x00, 0xEC, 0x95, 0x7B, 0xC5, 0x26, 0xF6, 0x6B, 0xC9,
  0x1F, 0x55, 0xBC, 0xDE, 0xBA, 0x35, 0xB4, 0x16, 0xCF, 0x30, 0x5E, 0x1E, 0xE1, 0xD0, 0x33, 0xBA,
  0x8E, 0x32, 0xC7, 0xB0, 0xF5, 0xDB, 0xD8, 0x1A, 0x45, 0xF8, 0xC3, 0xE2, 0xA9, 0x0E, 0xD8, 0xFC,
  0x07, 0x31, 0x63, 0xC0, 0xCC, 0xCD, 0xD7, 0xFE, 0xF8, 0xA6, 0xDE, 0x69, 0x3E, 0x21, 0xF1, 0xC4,
  0x10, 0x43, 0x76, 0x6C, 0xB4, 0xEB, 0xA1, 0x2B, 0x5C, 0xDF, 0x2F, 0xCE, 0x22, 0xDE, 0xC0, 0x2A,
  0x05, 0x6C, 0x10, 0xEC, 0xB1, 0xA8, 0x07, 0x07, 0xA9, 0x35, 0xC7, 0x8D, 0xA5, 0x56, 0x54, 0xFE,
  0x13, 0x0C, 0x4F, 0x2D, 0x4A, 0x76, 0x83, 0xBF, 0xF5, 0xFD, 0x32, 0x4F, 0x86, 0x76, 0xF1, 0x6B,
  0x9E, 0x23, 0xBC, 0xBA, 0x9E, 0xD9, 0x5E, 0xD6, 0xD6, 0x05, 0x08, 0x92, 0x0D, 0xE1, 0x5B, 0x70,
  0x2B, 0xD7, 0xA9, 0x1B, 0x49, 0xCF, 0xAD, 0x7B, 0x1D, 0x60, 0x78, 0x53, 0xC3, 0xF6, 0x1E, 0x15,
  0xD1, 0xD6, 0xC6, 0xDA, 0x75, 0x96, 0x46, 0x6D, 0xF3, 0x4C, 0x48, 0x06, 0x47, 0xF5, 0xC7, 0x61,
  0xD8, 0x0A, 0xDF, 0xAC, 0x30, 0xD4, 0x7D, 0x8D, 0x25, 0x03, 0x6C, 0x2D, 0x17, 0x4A, 0x92, 0x8B,
  0xDC, 0x28, 0xA2, 0x8A, 0xE8, 0x3A, 0x42, 0xB0, 0xFC, 0x59, 0xE1, 0x4D, 0x37, 0xC6, 0x3A, 0x24,
  0x9A, 0x66, 0xA4, 0x87, 0x69, 0x3B, 0xE2, 0x95, 0x3E, 0xFC, 0x4F, 0xD9, 0x97, 0xFC, 0x3B, 0x8A,
  0xDC, 0xA2, 0x9A, 0x6D, 0x3B, 0xA0, 0xDC, 0xF0, 0x6B, 0x5D, 0x13, 0xC5, 0xFF, 0x00, 0x0D, 0x26,
  0x7F, 0xED, 0x19, 0x6D, 0xEF, 0xBC, 0x2D, 0x6E, 0xAD, 0x23, 0xCE, 0x5B, 0xEE, 0x28, 0xE8, 0xAA,
  0x09, 0xDC, 0xAC, 0xC7, 0x0A, 0x07, 0x23, 0x26, 0xB8, 0x6D, 0x23, 0x4F, 0x3E, 0x37, 0x97, 0x5B,
  0xD4, 0x6F, 0x66, 0x91, 0xB5, 0x49, 0x25, 0x59, 0x17, 0xF7, 0x85, 0x12, 0x24, 0x27, 0xE7, 0x93,
  0xEE, 0x9D, 0xE1, 0x32, 0x83, 0x60, 0x20, 0xE0, 0xE4, 0x57, 0xB8, 0xFC, 0x54, 0xF0, 0x9E, 0xBF,
  0xE3, 0x2D, 0x3E, 0xCB, 0x4D, 0xD2, 0xA7, 0xB7, 0x8A, 0xD1, 0x24, 0x32, 0xDC, 0x09, 0x1B, 0x05,
  0xD8, 0x70, 0x83, 0xE8, 0x32, 0x4F, 0xD7, 0x1E, 0x95, 0xE5, 0xF7, 0x3F, 0x0A, 0xFC, 0x5D, 0xE1,
  0xDD, 0x12, 0xEA, 0x59, 0x75, 0xA3, 0x6F, 0xA6, 0xC7, 0x99, 0xA7, 0x4B, 0x56, 0x77, 0x1C, 0x0C,
  0x6E, 0x28, 0xA7, 0x9E, 0x3F, 0x4A, 0xDE, 0x85, 0x0A, 0x71, 0x6E, 0xA4, 0x52, 0x52, 0x96, 0xFB,
  0x6A, 0x71, 0xCE, 0x51, 0x84, 0x9E, 0x8F, 0xEE, 0x7F, 0x9D, 0x8C, 0x9F, 0xF8, 0x56, 0x37, 0x2E,
  0x58, 0xC5, 0xAD, 0xC2, 0xD1, 0x05, 0x0D, 0xE7, 0x34, 0x0E, 0x8A, 0x30, 0xFB, 0x58, 0x1C, 0xF3,
  0x95, 0x1C, 0xF0, 0x0F, 0xA5, 0x67, 0xB7, 0x86, 0xB4, 0xD8, 0x67, 0x99, 0x12, 0xF2, 0x6B, 0xB5,
  0x4B, 0x5B, 0x66, 0x56, 0x62, 0x60, 0xCC, 0xF3, 0x0D, 0xC0, 0x0E, 0x09, 0x23, 0x6E, 0x0E, 0x38,
  0xFA, 0x8A, 0xD1, 0x87, 0xC3, 0x5A, 0xF5, 0xDE, 0x99, 0x1C, 0xD6, 0x9E, 0x27, 0x9A, 0x7B, 0x1B,
  0xC8, 0x9E, 0x64, 0xD8, 0xD2, 0xE2, 0x40, 0xBB, 0xB7, 0x0D, 0xB9, 0xFB, 0xD9, 0xDD, 0xF2, 0xF5,
  0xE7, 0xDE, 0xB9, 0xC5, 0xBA, 0x03, 0x25, 0x7C, 0x4F, 0x72, 0xBB, 0x91, 0x10, 0xE2, 0x29, 0x79,
  0x55, 0x18, 0x51, 0xD7, 0xA0, 0x1C, 0x0F, 0x4A, 0xBA, 0x90, 0x9D, 0x58, 0x4A, 0x34, 0xE6, 0xAE,
  0xAD, 0xD5, 0x69, 0xB3, 0xFC, 0x8C, 0xDD, 0x68, 0x3D, 0x12, 0x7F, 0x73, 0xFF, 0x00, 0x22, 0xF4,
  0x5E, 0x0F, 0x90, 0x4D, 0x1B, 0x7D, 0xBE, 0x39, 0xE0, 0x6C, 0x8C, 0x80, 0xC9, 0xB8, 0xED, 0xDC,
  0x07, 0xAF, 0x4E, 0x7E, 0x83, 0xDE, 0x9A, 0x9E, 0x0E, 0x79, 0x30, 0xC9, 0xAB, 0x44, 0xD1, 0xEE,
  0x03, 0xCC, 0xF2, 0x9F, 0x1C, 0x90, 0x17, 0x1E, 0xB9, 0xFD, 0x07, 0xD6, 0x9D, 0x6F, 0xA2, 0xCA,
  0xF6, 0xD0, 0x5C, 0x37, 0x8A, 0x21, 0xB4, 0x8E, 0x5F, 0x9A, 0x0F, 0xB4, 0x99, 0x23, 0x2E, 0x38,
  0x5D, 0xC0, 0x73, 0x85, 0xE0, 0x0D, 0xC7, 0x03, 0x8F, 0x6A, 0xA9, 0x73, 0x01, 0xD3, 0xE5, 0xFB,
  0x15, 0xCF, 0x88, 0x2E, 0x21, 0x7B, 0x7F, 0x94, 0x45, 0xE4, 0xC9, 0x84, 0xE7, 0x77, 0xCB, 0x83,
  0x8C, 0x1E, 0xA0, 0x8E, 0x0F, 0x5A, 0xC3, 0xEA, 0xD8, 0xEF, 0xF9, 0xFA, 0xBE, 0xE4, 0x47, 0xB5,
  0x4F, 0xBF, 0xDC, 0xFF, 0x00, 0xC8, 0xB6, 0xBE, 0x0B, 0x94, 0xA0, 0x61, 0xAB, 0xC3, 0x85, 0x19,
  0x94, 0x94, 0x60, 0x23, 0x1F, 0x30, 0x3C, 0x9C, 0x03, 0xCA, 0xE2, 0x91, 0x7C, 0x1E, 0xDE, 0x5E,
  0x1F, 0x51, 0xDF, 0x2B, 0x15, 0x55, 0x11, 0x46, 0x48, 0x53, 0x9F, 0x9B, 0x24, 0x91, 0xC0, 0x50,
  0x4E, 0x78, 0xE9, 0x4C, 0xB0, 0xB1, 0x96, 0xF7, 0xCD, 0xB8, 0x87, 0xC4, 0xE6, 0x18, 0xA1, 0xFF,
  0x00, 0x5B, 0x73, 0x3A, 0xC9, 0x1A, 0x26, 0xEC, 0xF1, 0x92, 0x79, 0x27, 0x2D, 0xC0, 0xC9, 0xEB,
  0x45, 0xD5, 0xAC, 0xDA, 0x4C, 0x88, 0x5F, 0xC4, 0xD2, 0x05, 0x98, 0x6F, 0x8E, 0x6B, 0x75, 0x91,
  0xE3, 0x94, 0x64, 0xF2, 0x18, 0x1E, 0x70, 0x49, 0xC8, 0xEA, 0x09, 0xE6, 0x93, 0xC3, 0x63, 0xFF,
  0x00, 0xE7, 0xEA, 0xFB, 0x90, 0x7B, 0x55, 0x7B, 0x7E, 0x8F, 0xFC, 0x8B, 0xFA, 0x4F, 0x83, 0x61,
  0xD4, 0x35, 0x4B, 0xDD, 0x39, 0xEE, 0x24, 0x59, 0x6D, 0xB4, 0xE3, 0x28, 0x90, 0x36, 0x14, 0xCE,
  0x5B, 0xE5, 0xCE, 0x7F, 0x87, 0x68, 0x27, 0x1E, 0xD5, 0xA1, 0xA8, 0x7C, 0x33, 0x07, 0x51, 0xB9,
  0x16, 0x3A, 0xB8, 0x86, 0xD1, 0x64, 0xD9, 0x1A, 0xCF, 0x1B, 0xB3, 0x8C, 0xE3, 0xCB, 0x04, 0x8C,
  0x02, 0x5B, 0x24, 0x9C, 0x7D, 0xDE, 0xF5, 0xCD, 0x59, 0x59, 0xB6, 0xA5, 0x74, 0xD0, 0x5A, 0x6B,
  0xF3, 0x3C, 0xB2, 0x00, 0xCE, 0x4C, 0x4E, 0xA3, 0x6A, 0x03, 0xF3, 0x3B, 0x13, 0x80, 0xAA, 0x09,
  0xE4, 0xF4, 0xAB, 0xF7, 0x76, 0xD7, 0x90, 0xDA, 0x25, 0xD9, 0xF1, 0x91, 0xBA, 0xB5, 0x85, 0xF6,
  0x79, 0xB6, 0xCF, 0x2C, 0xA2, 0x26, 0x24, 0x9E, 0x46, 0x41, 0x5C, 0x92, 0x48, 0x38, 0xC1, 0xE7,
  0x9A, 0xEC, 0xA7, 0x4A, 0xAC, 0x62, 0x94, 0xA5, 0x76, 0x68, 0xAB, 0x41, 0x2B, 0x34, 0xFE, 0xE7,
  0xFE, 0x46, 0xA4, 0xFF, 0x00, 0x0D, 0x84, 0xF3, 0x45, 0x1D, 0x9E, 0xAA, 0xB1, 0x48, 0xD1, 0xE6,
  0x45, 0x95, 0x1D, 0x95, 0x58, 0x31, 0x0D, 0x86, 0xC7, 0x40, 0xA3, 0x77, 0x3D, 0x49, 0xC0, 0xA8,
  0x87, 0xC3, 0x93, 0x75, 0x0C, 0x12, 0xDB, 0x6A, 0xF1, 0xAA, 0x18, 0xA3, 0x91, 0xFF, 0x00, 0x73,
  0x2B, 0x33, 0x23, 0x03, 0x89, 0x31, 0x8E, 0xE7, 0x03, 0x67, 0x51, 0xC9, 0xAC, 0x74, 0xBA, 0x33,
  0x22, 0xDB, 0xAF, 0x8C, 0xAF, 0x98, 0x48, 0x42, 0x88, 0x96, 0x09, 0xCE, 0xE3, 0x93, 0x81, 0x8C,
  0xF3, 0xCB, 0x1F, 0xC4, 0x9A, 0xDC, 0xB1, 0xB5, 0xD4, 0xB4, 0x3B, 0xD9, 0x2E, 0x60, 0xF1, 0x45,
  0xA5, 0xD5, 0xE5, 0xAC, 0x67, 0x75, 0xB6, 0x5E, 0x59, 0x20, 0x00, 0x1E, 0x76, 0x06, 0x00, 0x94,
  0x04, 0xF0, 0x09, 0xC6, 0x4E, 0x47, 0x5A, 0xAE, 0x49, 0xF7, 0x1B, 0xAB, 0x05, 0xBA, 0x7F, 0x73,
  0x29, 0xE9, 0xDE, 0x0B, 0x8B, 0x54, 0xD1, 0xA6, 0x16, 0x97, 0x26, 0x4D, 0x42, 0x2B, 0xC9, 0x62,
  0x92, 0x66, 0x90, 0xAC, 0x68, 0x88, 0x09, 0x04, 0x21, 0x1F, 0x30, 0x65, 0x49, 0x1B, 0x70, 0x6E,
  0x36, 0x60, 0x8A, 0x9E, 0x3F, 0x87, 0x29, 0x25, 0xA1, 0x91, 0x75, 0xE8, 0xC9, 0xF9, 0x64, 0x59,
  0x8C, 0x45, 0x61, 0x30, 0xEC, 0x04, 0xB0, 0x62, 0x79, 0xF9, 0x9D, 0x17, 0xB7, 0xF1, 0x71, 0x91,
  0x8A, 0xC9, 0xB4, 0xD4, 0x63, 0xB6, 0xB4, 0x6B, 0x51, 0xE2, 0x97, 0x68, 0x0C, 0x52, 0x45, 0x1A,
  0x49, 0x6B, 0x2B, 0x2C, 0x22, 0x4F, 0xBE, 0x50, 0x67, 0x0A, 0xCC, 0x09, 0x04, 0xFA, 0x31, 0xF5,
  0xAD, 0x26, 0xB6, 0xBA, 0x16, 0x0D, 0xA8, 0x3F, 0x8E, 0x2E, 0xC4, 0x6D, 0x20, 0x84, 0x66, 0x0B,
  0x8C, 0xC8, 0xCC, 0xB8, 0xDA, 0x07, 0x7F, 0x94, 0x00, 0x7D, 0xB1, 0x47, 0x24, 0xFA, 0x30, 0xF6,
  0xB4, 0xFA, 0xFE, 0xA6, 0x67, 0x88, 0xBC, 0x22, 0xFE, 0x1F, 0xD3, 0x63, 0xBA, 0x6D, 0x5A, 0x2B,
  0x96, 0x69, 0x44, 0x4F, 0x14, 0x71, 0xBA, 0x94, 0x6F, 0x9F, 0x3C, 0x9E, 0x0E, 0x0C, 0x6C, 0x38,
  0xF6, 0x35, 0xF4, 0xC7, 0xC3, 0xCD, 0x4A, 0xFF, 0x00, 0x56, 0xF0, 0x16, 0x91, 0x79, 0xA9, 0x46,
  0xC9, 0x74, 0xF0, 0x05, 0x62, 0xDD, 0x64, 0x03, 0x80, 0xFF, 0x00, 0xF0, 0x20, 0x01, 0xFC, 0x6B,
  0xC9, 0xB4, 0xFF, 0x00, 0x82, 0xBE, 0x23, 0xBE, 0xD4, 0x6C, 0x9B, 0x58, 0xD4, 0xC4, 0xDA, 0x6F,
  0x98, 0xB2, 0x4A, 0xAE, 0xED, 0xBF, 0x6F, 0x53, 0xF2, 0x93, 0xC1, 0x39, 0x23, 0xF1, 0x35, 0xEF,
  0xB1, 0x45, 0x1C, 0x10, 0xA4, 0x31, 0x20, 0x48, 0xE3, 0x50, 0xAA, 0xA0, 0x70, 0x00, 0xE0, 0x0A,
  0xC2, 0xBB, 0x5C, 0xA9, 0x5E, 0xEC, 0xD6, 0x8B, 0x72, 0x77, 0x4A, 0xCB, 0xFA, 0xEE, 0x3E, 0x8A,
  0x28, 0xAE, 0x53, 0xA4, 0x28, 0xA2, 0x8A, 0x00, 0x29, 0x19, 0x55, 0xD1, 0x91, 0xD4, 0x32, 0xB0,
  0xC1, 0x52, 0x32, 0x08, 0xA5, 0xA2, 0x80, 0x3C, 0x44, 0xE9, 0xBF, 0xF0, 0x82, 0x78, 0xCA, 0x4F,
  0x0D, 0xCC, 0xFE, 0x56, 0x89, 0xAB, 0x4D, 0xF6, 0xAD, 0x1E, 0xE5, 0xBA, 0x5B, 0x5C, 0xF7, 0x8C,
  0x9E, 0xC0, 0xF0, 0x3F, 0xEF, 0x83, 0xEB, 0x5C, 0x47, 0xC4, 0x0F, 0x07, 0x35, 0x9E, 0xA6, 0x35,
  0x4B, 0x58, 0x4C, 0x76, 0x77, 0x53, 0x88, 0xEE, 0xA2, 0x51, 0x8F, 0xB2, 0xCC, 0x4E, 0x08, 0xC7,
  0x65, 0x6E, 0x48, 0xF4, 0x39, 0x1D, 0x85, 0x7D, 0x0D, 0xE3, 0x1F, 0x0A, 0x59, 0x78, 0xCB, 0xC3,
  0xB3, 0xE9, 0x57, 0x9F, 0x29, 0x6F, 0x9E, 0x09, 0x80, 0xCB, 0x43, 0x20, 0xE8, 0xC3, 0xF9, 0x11,
  0xDC, 0x13, 0x5E, 0x69, 0xA1, 0xEA, 0x37, 0x17, 0xD7, 0x37, 0x3E, 0x0F, 0xF1, 0x44, 0x6A, 0x9E,
  0x20, 0x82, 0x23, 0x6E, 0x7C, 0xCE, 0x53, 0x52, 0x83, 0x1C, 0x10, 0x4F, 0x57, 0x00, 0x02, 0x0F,
  0x53, 0x80, 0x7A, 0x83, 0x59, 0xCD, 0xCE, 0x95, 0x4F, 0xAC, 0xD3, 0x57, 0xE9, 0x25, 0xDD, 0x77,
  0x5E, 0x6B, 0xF1, 0x47, 0x35, 0x48, 0x5B, 0x54, 0x71, 0x5E, 0x1D, 0x5B, 0x9B, 0xEB, 0xBF, 0x14,
  0x32, 0x07, 0x11, 0xB9, 0xFB, 0x22, 0xC6, 0xA0, 0x62, 0x48, 0xD5, 0x25, 0x55, 0x87, 0xE9, 0xC4,
  0x7C, 0x7A, 0x85, 0xAE, 0x54, 0xDA, 0xFF, 0x00, 0x6B, 0x69, 0x3A, 0x74, 0xB2, 0x5E, 0x5A, 0x5A,
  0xBC, 0x4B, 0x24, 0x01, 0xAE, 0xA4, 0x2A, 0x19, 0x15, 0x81, 0x50, 0x0E, 0x0E, 0x71, 0xBC, 0x8F,
  0xA0, 0x15, 0xDD, 0x47, 0x3D, 0xCF, 0x85, 0x2F, 0x75, 0x38, 0x64, 0x8E, 0xDC, 0x3D, 0xC4, 0x9E,
  0x69, 0x17, 0x17, 0x02, 0x00, 0xB2, 0xE3, 0x05, 0xFE, 0x60, 0x77, 0xC4, 0xC4, 0x2B, 0x61, 0x70,
  0xC0, 0x82, 0xB5, 0xE7, 0x7A, 0xA9, 0x46, 0x36, 0xF6, 0xD6, 0xCC, 0xD2, 0x5B, 0xDB, 0x21, 0x51,
  0x29, 0x5D, 0xBE, 0x6B, 0x93, 0x97, 0x7C, 0x76, 0x04, 0xF0, 0x3D, 0x80, 0xAF, 0x4A, 0x94, 0x9C,
  0x9C, 0xA7, 0xA7, 0x2B, 0xB5, 0x99, 0x83, 0x9C, 0x1C, 0x62, 0x96, 0xFD, 0x4E, 0x8B, 0x43, 0xD2,
  0x05, 0xEE, 0xA7, 0xA3, 0x69, 0xA5, 0xE2, 0x9A, 0xD2, 0xDD, 0x25, 0xBF, 0x9C, 0xC6, 0xD9, 0x49,
  0x48, 0x72, 0xBC, 0x7A, 0xFD, 0xD4, 0x1F, 0x4C, 0xD5, 0x6D, 0x4B, 0x49, 0xFB, 0x3C, 0xDA, 0xB6,
  0x92, 0x67, 0x8A, 0x18, 0x22, 0xB9, 0x8A, 0xE6, 0xDE, 0x49, 0xD8, 0xAA, 0x46, 0x24, 0xC8, 0x3C,
  0xE0, 0xE3, 0x23, 0x03, 0xFE, 0x02, 0x2A, 0x1F, 0x0F, 0x6A, 0x8D, 0x67, 0xE4, 0x94, 0x9E, 0x18,
  0x2E, 0xAD, 0xF7, 0xA4, 0x7F, 0x68, 0x25, 0x62, 0x9A, 0x27, 0x39, 0x64, 0x66, 0x1F, 0x74, 0x86,
  0xE4, 0x1E, 0x9C, 0x9A, 0x4F, 0x11, 0x6A, 0x12, 0xDF, 0x34, 0x8C, 0xF2, 0xC7, 0x3C, 0xF7, 0x52,
  0x2C, 0x93, 0x4B, 0x0A, 0x91, 0x10, 0x08, 0x36, 0xA4, 0x71, 0x93, 0xF7, 0x82, 0xE4, 0xE4, 0xFA,
  0xFD, 0x2B, 0xA9, 0xB8, 0x2A, 0x7E, 0x67, 0x12, 0x55, 0x7E, 0xB2, 0xE4, 0xDF, 0xBB, 0x6F, 0xC6,
  0xF7, 0xBF, 0xDC, 0x4B, 0xA7, 0x69, 0x09, 0xF6, 0x39, 0xAC, 0xBF, 0xB4, 0xAD, 0x88, 0xBC, 0x9D,
  0x22, 0x92, 0xE2, 0xD8, 0xF9, 0x8A, 0x11, 0x55, 0x9F, 0x67, 0x25, 0x79, 0x2C, 0x17, 0x8C, 0x8E,
  0x82, 0xB6, 0xF5, 0x1D, 0x3E, 0x33, 0x7D, 0xB8, 0x3C, 0x28, 0xB2, 0xC0, 0x6D, 0x24, 0x11, 0xC5,
  0x1C, 0x6B, 0x70, 0xAC, 0x24, 0x65, 0xDA, 0xA8, 0xED, 0x8F, 0x2F, 0x6A, 0x72, 0x79, 0x3B, 0x72,
  0x4F, 0xAF, 0x2D, 0xA2, 0x34, 0x88, 0x93, 0xC0, 0x64, 0x7B, 0x60, 0x59, 0x66, 0x8A, 0xF0, 0x29,
  0x2B, 0x6F, 0x2A, 0xE7, 0x0C, 0xD8, 0x07, 0x0A, 0x41, 0x20, 0x9E, 0xD9, 0x07, 0xB5, 0x69, 0x6A,
  0x9A, 0x83, 0x04, 0x4B, 0xAB, 0xA9, 0x2D, 0x1A, 0xFD, 0x2D, 0xDE, 0x08, 0x96, 0xD6, 0x6F, 0x37,
  0x73, 0x3E, 0x77, 0xCC, 0xC4, 0x70, 0xA3, 0x04, 0xE1, 0x47, 0x72, 0x4E, 0x07, 0x35, 0x8A, 0xD5,
  0xDD, 0x3D, 0x8E, 0xCE, 0x75, 0x66, 0xBA, 0xB2, 0x8E, 0x9B, 0x67, 0xFD, 0x99, 0x24, 0xB7, 0xF1,
  0xDF, 0xD9, 0x5C, 0x4D, 0x05, 0xAC, 0x92, 0x44, 0x2D, 0xA5, 0x2C, 0xC8, 0xF8, 0xC0, 0x63, 0x90,
  0x31, 0x8D, 0xD9, 0xFA, 0x81, 0x5A, 0x5A, 0x26, 0x91, 0x71, 0xA6, 0xE9, 0xF7, 0x9A, 0xBC, 0x23,
  0xE7, 0x8A, 0xC9, 0xE7, 0x8A, 0x69, 0x22, 0x0C, 0x99, 0x47, 0x00, 0xF9, 0x67, 0x39, 0xC9, 0x20,
  0x8C, 0xE3, 0xA2, 0xB7, 0x5C, 0x8A, 0xE7, 0x34, 0xB9, 0xD6, 0xCA, 0xFD, 0x1E, 0x68, 0x9A, 0x4B,
  0x77, 0x06, 0x29, 0x51, 0x3E, 0xF3, 0x23, 0x0C, 0x1C, 0x7B, 0xF7, 0x1E, 0xE0, 0x57, 0xA0, 0x0D,
  0x63, 0x52, 0xD5, 0x6E, 0x64, 0xB5, 0x73, 0xA6, 0x5D, 0xDB, 0x5D, 0x06, 0x8D, 0x62, 0xB3, 0x2C,
  0x66, 0x9F, 0x73, 0x16, 0xDA, 0x23, 0xCE, 0x50, 0xE4, 0xE0, 0xEE, 0xC0, 0x03, 0x3D, 0x7B, 0xE9,
  0x1D, 0x4C, 0xAA, 0xBD, 0x7C, 0x8E, 0x6B, 0x4F, 0xF0, 0xCC, 0xBA, 0xB7, 0x8C, 0x2E, 0x2C, 0xAC,
  0xE1, 0x1E, 0x52, 0xDC, 0xB0, 0x45, 0x3C, 0x28, 0xE7, 0x3C, 0x9E, 0xCA, 0xA3, 0x92, 0x7D, 0x05,
  0x7A, 0x8F, 0x81, 0xF4, 0x3B, 0x7F, 0x16, 0x78, 0xAA, 0x0B, 0xB8, 0x57, 0xCC, 0xF0, 0xC7, 0x86,
  0xC9, 0x8A, 0xC9, 0x99, 0x70, 0x2F, 0x2E, 0x8F, 0x2F, 0x31, 0x1F, 0x5C, 0x1F, 0xA0, 0x41, 0xEB,
  0x58, 0xB0, 0xE9, 0x97, 0x77, 0xB7, 0xCD, 0xE0, 0xBD, 0x0A, 0x44, 0x7D, 0x56, 0xEC, 0x13, 0xAE,
  0x6A, 0x31, 0x73, 0x1D, 0xA4, 0x44, 0xE5, 0xE2, 0x53, 0xDC, 0x93, 0xF7, 0xBD, 0x4E, 0x17, 0xD7,
  0x1E, 0xEB, 0xA2, 0x68, 0xB6, 0x5E, 0x1E, 0xD1, 0xAD, 0x74, 0xAD, 0x3A, 0x2F, 0x2E, 0xD6, 0xD9,
  0x36, 0x28, 0xEE, 0x7D, 0x49, 0xF5, 0x24, 0xE4, 0x9F, 0xAD, 0x73, 0x62, 0x2A, 0xD9, 0x72, 0xC4,
  0xE9, 0xC3, 0x52, 0x93, 0xF7, 0xE7, 0xFD, 0x7F, 0x5D, 0x4D, 0x0A, 0x28, 0xA2, 0xB8, 0x8E, 0xD0,
  0xA2, 0x8A, 0x28, 0x00, 0xA2, 0x8A, 0x28, 0x00, 0xA2, 0x8A, 0x28, 0x00, 0xAE, 0x4B, 0xC7, 0x5E,
  0x04, 0xB2, 0xF1, 0xA6, 0x9F, 0x19, 0xF3, 0x0D, 0xA6, 0xAB, 0x6B, 0xF3, 0x59, 0xDF, 0x47, 0xC3,
  0x46, 0xDD, 0x70, 0x71, 0xC9, 0x5C, 0xFE, 0x5D, 0x45, 0x75, 0xB4, 0x53, 0x4D, 0xA7, 0x74, 0x0D,
  0x5C, 0xF1, 0x4B, 0x0F, 0x16, 0x5C, 0x69, 0xFA, 0x92, 0x78, 0x7F, 0xC7, 0x91, 0x2E, 0x9D, 0xAC,
  0x47, 0xC4, 0x3A, 0x93, 0xA8, 0x30, 0x5D, 0x2F, 0x40, 0x5C, 0xE3, 0x1C, 0xFF, 0x00, 0x7B, 0xA1,
  0xEE, 0x01, 0xAB, 0x3A, 0xDF, 0x80, 0xF4, 0x7B, 0x9F, 0x9F, 0xEC, 0xE7, 0x4F, 0x95, 0xC6, 0xE0,
  0x61, 0x5D, 0xF0, 0x3E, 0x7B, 0xA8, 0x1D, 0x3F, 0x0C, 0x8F, 0x6A, 0xF4, 0xDF, 0x10, 0x78, 0x6B,
  0x49, 0xF1, 0x46, 0x9A, 0xD6, 0x1A, 0xBD, 0x9A, 0x5C, 0x43, 0xD5, 0x49, 0xE1, 0xA3, 0x3E, 0xAA,
  0xC3, 0x90, 0x6B, 0xCB, 0xE6, 0xF0, 0x4F, 0x8E, 0x7C, 0x0A, 0x1B, 0xFE, 0x11, 0x5B, 0xE4, 0xD7,
  0x34, 0x60, 0x73, 0xFD, 0x99, 0x7A, 0x01, 0x65, 0x1E, 0x8A, 0x0E, 0x07, 0xE2, 0xA4, 0x7D, 0x2B,
  0x19, 0xD0, 0xBB, 0x73, 0xA1, 0x2E, 0x49, 0x3D, 0xFB, 0x3F, 0x91, 0xC5, 0x5F, 0x0A, 0xA6, 0x8E,
  0x5A, 0xE3, 0xE1, 0x93, 0xB6, 0x4D, 0xBE, 0xA5, 0xA7, 0x9F, 0x40, 0xF2, 0x32, 0xFF, 0x00, 0xEC,
  0xB5, 0x3C, 0x5A, 0x33, 0x59, 0xF8, 0x56, 0xE7, 0x42, 0xD5, 0x21, 0x8F, 0xCD, 0x8C, 0xC9, 0x35,
  0xB4, 0x8A, 0xC1, 0xC3, 0x0E, 0x09, 0x64, 0x23, 0xD3, 0x9C, 0xFE, 0xBD, 0x6B, 0x48, 0x7C, 0x40,
  0xD1, 0x16, 0x63, 0x6D, 0xE2, 0x4D, 0x0F, 0x56, 0xD0, 0x6F, 0x3A, 0x30, 0x11, 0x97, 0x4C, 0xFD,
  0x08, 0x0D, 0xFA, 0x1A, 0x9C, 0x5F, 0xF8, 0x36, 0xF2, 0xE2, 0xDE, 0xE2, 0x2F, 0x16, 0x59, 0x62,
  0x29, 0x04, 0x81, 0x6E, 0x23, 0x68, 0xD8, 0x7A, 0x8E, 0x7D, 0x47, 0x07, 0xEB, 0x5E, 0x7D, 0x7A,
  0x19, 0x8D, 0x56, 0xA1, 0x56, 0x0A, 0xC9, 0xA6, 0x9C, 0x5E, 0xD6, 0xEB, 0xAB, 0x38, 0xFD, 0x8C,
  0xA0, 0xAC, 0x8C, 0x9D, 0x5B, 0xC9, 0x87, 0x47, 0x7F, 0x0F, 0x69, 0xD1, 0xA2, 0x1D, 0xA2, 0x37,
  0xCB, 0x04, 0x5C, 0xF0, 0x5C, 0xBB, 0x1E, 0x06, 0x3B, 0x93, 0xDE, 0xB9, 0x45, 0xF0, 0xF6, 0x9F,
  0x04, 0x81, 0x6E, 0x75, 0xBB, 0x1E, 0xBC, 0x8B, 0x75, 0x92, 0x53, 0xF8, 0x7C, 0xA0, 0x13, 0xF8,
  0xD7, 0x41, 0x2E, 0x8F, 0xE1, 0x11, 0x34, 0x97, 0x1A, 0x9F, 0x8F, 0x6D, 0xA5, 0x0E, 0xC5, 0xDA,
  0x3B, 0x58, 0x8B, 0x12, 0x49, 0xCF, 0xBF, 0xF2, 0xA9, 0xED, 0x3C, 0x43, 0xE1, 0x7B, 0x49, 0xBE,
  0xCD, 0xE1, 0x5F, 0x0E, 0xEA, 0x7A, 0xF5, 0xF1, 0xE1, 0x59, 0xA3, 0x20, 0x7E, 0x24, 0x02, 0xD8,
  0xFA, 0x05, 0xAE, 0x9C, 0x16, 0x5F, 0x5A, 0x8A, 0x71, 0xBD, 0x93, 0x77, 0x6F, 0x77, 0xFD, 0x7A,
  0x90, 0xE1, 0x55, 0xEC, 0x8A, 0xDA, 0x2F, 0x84, 0x16, 0xF6, 0x62, 0xD6, 0xF6, 0xD2, 0x41, 0x6A,
  0x83, 0x74, 0x97, 0x13, 0x90, 0xB2, 0x15, 0xEF, 0x93, 0xF7, 0x62, 0x5F, 0x7E, 0xB5, 0xA5, 0x6F,
  0x73, 0x36, 0xAB, 0xA8, 0x37, 0x87, 0x7E, 0x1F, 0xC4, 0x8F, 0x71, 0xB7, 0xCB, 0xBB, 0xD6, 0x55,
  0x36, 0xC3, 0x6B, 0x19, 0xEA, 0x22, 0x3D, 0x7D, 0x7E, 0x6F, 0xBC, 0x7B, 0x7A, 0xD6, 0xCD, 0xA7,
  0xC3, 0xFF, 0x00, 0x17, 0xF8, 0xD7, 0xCB, 0x6F, 0x17, 0x5E, 0x26, 0x8F, 0xA3, 0x82, 0x19, 0x74,
  0x9B, 0x0C, 0x29, 0x61, 0xFE, 0xD6, 0x32, 0x07, 0xD4, 0x96, 0x3E, 0xC2, 0xBD, 0x5B, 0x45, 0xD0,
  0xB4, 0xCF, 0x0E, 0xE9, 0xB1, 0xE9, 0xFA, 0x55, 0x9C, 0x76, 0xD6, 0xC9, 0xFC, 0x28, 0x39, 0x63,
  0xEA, 0xC7, 0xA9, 0x3E, 0xE6, 0xBD, 0x48, 0xBA, 0x74, 0x63, 0xCB, 0x4F, 0x56, 0xF7, 0x6F, 0x56,
  0xFE, 0x7F, 0xA2, 0xD0, 0xE9, 0xC3, 0xE0, 0x5A, 0x6A, 0x75, 0x9D, 0xDF, 0xE0, 0xBF, 0xAE, 0xE6,
  0x77, 0x83, 0xBC, 0x1D, 0xA6, 0xF8, 0x2F, 0x45, 0x5B, 0x0B, 0x05, 0x2F, 0x23, 0x9D, 0xF7, 0x17,
  0x2E, 0x3E, 0x79, 0xDF, 0xD4, 0xFB, 0x7A, 0x0E, 0xDF, 0x99, 0xAE, 0x86, 0x8A, 0x2B, 0x06, 0xDB,
  0x77, 0x67, 0xA7, 0xB0, 0x51, 0x45, 0x14, 0x80, 0x28, 0xA2, 0x8A, 0x00, 0xFF, 0xD9
};

const char INDEX_HTML[] PROGMEM = R"SEMS_HTML(<!doctype html>
<html lang="en" class="scroll-smooth">

<head>
    <script>window["__codeletBootstrap__"] = JSON.parse('{"A":"A","B":"20261112-04-ec29115","C":{}}');</script>
    <script src="/_sdk/829b2c3477130542.telemetry_sdk.js" onerror="this.remove()"></script>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SEMS | Smart Environmental Monitoring System</title>
    <link rel="icon" type="image/jpeg" href="sems-logo.jpg">

    <!-- Google Fonts: Plus Jakarta Sans & JetBrains Mono -->
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700;800&family=JetBrains+Mono:wght@400;500;600;700&display=swap" rel="stylesheet">

    <!-- Tailwind CSS with Dark Mode Class Strategy -->
    <script src="https://cdn.tailwindcss.com/3.4.17"></script>
    <script>
        tailwind.config = {
            darkMode: 'class',
            theme: {
                extend: {
                    fontFamily: {
                        sans: ['"Plus Jakarta Sans"', 'system-ui', '-apple-system', 'sans-serif'],
                        mono: ['"JetBrains Mono"', 'ui-monospace', 'monospace']
                    },
                    colors: {
                        brand: {
                            50: '#ecfdf5',
                            100: '#d1fae5',
                            500: '#10b981',
                            600: '#059669',
                            700: '#047857',
                            900: '#064e3b'
                        }
                    }
                }
            }
        };
    </script>

    <!-- Chart.js 4 and Lucide Icons -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.8/dist/chart.umd.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/lucide@latest/dist/umd/lucide.min.js"></script>

    <!-- Optional Editor SDKs (fail-safe) -->
    <script src="/_sdk/ea2423e0a01a756f.data_sdk.js" onerror="this.remove()"></script>
    <script src="/_sdk/dc09d1c476e4a0aa.editing_sdk.js" onerror="this.remove()"></script>
    <script src="/_sdk/a202d01287f70bbd.resizing_sdk.js" onerror="this.remove()"></script>

    <style>
        :root {
            --color-bg: #f8fafc;
            --color-surface: #ffffff;
            --color-border: #e2e8f0;
            --color-text-main: #0f172a;
            --color-text-muted: #475569;
            --color-text-subtle: #64748b;
        }

        .dark {
            --color-bg: #0b0f19;
            --color-surface: #111827;
            --color-border: #1e293b;
            --color-text-main: #f8fafc;
            --color-text-muted: #cbd5e1;
            --color-text-subtle: #94a3b8;
        }

        body {
            font-family: 'Plus Jakarta Sans', system-ui, -apple-system, sans-serif;
            background-color: var(--color-bg);
            color: var(--color-text-main);
            transition: background-color 0.25s ease, color 0.25s ease;
        }

        .mono {
            font-family: 'JetBrains Mono', monospace;
        }

        .custom-scrollbar::-webkit-scrollbar {
            width: 6px;
            height: 6px;
        }

        .custom-scrollbar::-webkit-scrollbar-track {
            background: transparent;
        }

        .custom-scrollbar::-webkit-scrollbar-thumb {
            background-color: rgba(148, 163, 184, 0.3);
            border-radius: 9999px;
        }

        .custom-scrollbar::-webkit-scrollbar-thumb:hover {
            background-color: rgba(148, 163, 184, 0.5);
        }

        /* Pulse animation for live indicators */
        @keyframes pulse-ring {
            0% { transform: scale(0.95); opacity: 0.8; }
            50% { transform: scale(1.35); opacity: 0; }
            100% { transform: scale(0.95); opacity: 0; }
        }

        .ping-ring {
            animation: pulse-ring 2s cubic-bezier(0.4, 0, 0.6, 1) infinite;
        }

        /* Ambient subtle gradient background */
        .ambient-glow {
            background: radial-gradient(circle at 10% 10%, rgba(16, 185, 129, 0.08) 0%, transparent 45%),
                        radial-gradient(circle at 90% 20%, rgba(14, 165, 233, 0.08) 0%, transparent 45%),
                        radial-gradient(circle at 50% 90%, rgba(245, 158, 11, 0.05) 0%, transparent 50%);
        }

        .dark .ambient-glow {
            background: radial-gradient(circle at 10% 10%, rgba(16, 185, 129, 0.12) 0%, transparent 45%),
                        radial-gradient(circle at 90% 20%, rgba(14, 165, 233, 0.12) 0%, transparent 45%),
                        radial-gradient(circle at 50% 90%, rgba(245, 158, 11, 0.08) 0%, transparent 50%);
        }
    </style>
</head>

<body data-template-id="__page-root" class="min-h-screen antialiased selection:bg-emerald-500 selection:text-white">
    <!-- Floating Website Threshold Alert Notification -->
    <div id="web-alert-toast" role="alert" aria-live="assertive"
        class="hidden fixed top-20 right-4 z-50 max-w-sm w-[calc(100%-2rem)] rounded-2xl border-2 border-red-500 bg-red-50 dark:bg-red-950/95 text-red-950 dark:text-red-50 shadow-2xl p-4 backdrop-blur-md transition-all">
        <div class="flex items-start gap-3">
            <div class="p-2 rounded-xl bg-red-600 text-white shrink-0">
                <i data-lucide="alert-triangle" class="w-4 h-4"></i>
            </div>
            <div class="flex-1 min-w-0">
                <p id="web-alert-title" class="text-xs font-extrabold uppercase tracking-wide text-red-700 dark:text-red-300">Threshold Exceeded</p>
                <p id="web-alert-msg" class="text-xs font-medium mt-0.5 leading-snug"></p>
                <p id="web-alert-meta" class="text-[11px] text-red-700/80 dark:text-red-300/80 mono mt-1"></p>
            </div>
            <button id="web-alert-close" type="button" aria-label="Dismiss notification"
                class="p-1 rounded-lg hover:bg-red-200/70 dark:hover:bg-red-900/70 text-red-700 dark:text-red-300 transition">
                <i data-lucide="x" class="w-4 h-4"></i>
            </button>
        </div>
    </div>

    <div class="ambient-glow min-h-screen flex flex-col">

        <!-- Top Global Navigation Bar -->
        <header data-template-id="site-header" class="sticky top-0 z-40 w-full backdrop-blur-md bg-white/90 dark:bg-slate-900/90 border-b border-slate-200 dark:border-slate-800 transition-colors duration-200">
            <div class="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-3.5 flex flex-wrap items-center justify-between gap-4">
                
                <!-- Brand & Architecture Identity -->
                <div class="flex items-center gap-3.5">
                    <div class="relative flex items-center justify-center shrink-0">
                        <img src="sems-logo.jpg" alt="SEMS Logo" class="w-11 h-11 rounded-xl object-cover border border-emerald-500/30 shadow-md shadow-emerald-500/20 bg-white dark:bg-slate-800">
                        <span class="absolute -top-1 -right-1 flex h-3 w-3">
                            <span id="header-pulse" class="ping-ring absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75"></span>
                            <span class="relative inline-flex rounded-full h-3 w-3 bg-emerald-500 border-2 border-white dark:border-slate-900"></span>
                        </span>
                    </div>
                    <div>
                        <div class="flex items-center gap-2">
                            <span data-template-id="brand-name" class="font-extrabold text-slate-900 dark:text-white text-lg tracking-tight">SEMS</span>
                            <span class="inline-flex items-center px-2 py-0.5 rounded text-xs font-semibold bg-emerald-100 text-emerald-800 dark:bg-emerald-950/70 dark:text-emerald-300 border border-emerald-200 dark:border-emerald-800/60">
                                ESP32 Gateway &bull; v2.4
                            </span>
                        </div>
                        <span data-template-id="brand-caption" class="block text-xs font-medium text-slate-500 dark:text-slate-400">
                            Smart Environmental Monitoring &amp; Telegram Incident Dispatch
                        </span>
                    </div>
                </div>

                <!-- Right Action Center -->
                <div class="flex items-center flex-wrap gap-2.5 sm:gap-3">
                    
                    <!-- Live Clock (Desktop) -->
                    <div class="hidden lg:flex items-center gap-1.5 px-3 py-1.5 rounded-xl bg-slate-100 dark:bg-slate-800/80 text-slate-700 dark:text-slate-300 border border-slate-200 dark:border-slate-700 text-xs font-medium mono">
                        <i data-lucide="clock" class="w-3.5 h-3.5 text-slate-400"></i>
                        <span id="live-clock">--:--:--</span>
                    </div>

                    <!-- Audio Alert Toggle -->
                    <button id="audio-toggle" type="button" title="Toggle audio alert chime"
                        class="p-2 rounded-xl text-slate-600 dark:text-slate-300 bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 border border-slate-200 dark:border-slate-700 transition">
                        <i id="audio-icon" data-lucide="volume-2" class="w-4 h-4 text-emerald-600 dark:text-emerald-400"></i>
                        <span class="sr-only">Toggle alert sound</span>
                    </button>

                    <!-- ESP32 Hardware Connection Status Pill -->
                    <button id="esp-status-btn" type="button" title="Configure ESP32 IP address or connection"
                        class="flex items-center gap-1.5 px-3 py-1.5 rounded-xl bg-slate-100 dark:bg-slate-800 hover:bg-slate-200/80 dark:hover:bg-slate-700 border border-slate-200 dark:border-slate-700 text-xs font-semibold text-slate-700 dark:text-slate-300 transition">
                        <span id="esp-status-dot" class="w-2.5 h-2.5 rounded-full bg-amber-500"></span>
                        <span id="esp-status-text">ESP32: Auto</span>
                    </button>

                    <!-- Simulation Status Pill -->
                    <div class="flex items-center gap-2 px-3 py-1.5 rounded-xl bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 text-xs font-medium">
                        <span id="status-dot" class="w-2.5 h-2.5 rounded-full bg-emerald-500 ring-4 ring-emerald-500/20"></span>
                        <span id="status-running" data-template-id="status-running" class="text-emerald-700 dark:text-emerald-400 font-semibold">Live stream</span>
                        <span id="status-paused" data-template-id="status-paused" class="text-amber-600 dark:text-amber-400 font-semibold hidden">Stream paused</span>
                    </div>

                    <!-- Simulation Run/Pause Toggle Button -->
                    <button id="toggle-simulation" type="button"
                        class="flex items-center gap-1.5 px-3 py-1.5 rounded-xl text-xs font-semibold bg-white dark:bg-slate-800 hover:bg-slate-100 dark:hover:bg-slate-700 border border-slate-300 dark:border-slate-700 text-slate-700 dark:text-slate-200 shadow-sm transition">
                        <i id="sim-icon" data-lucide="pause" class="w-3.5 h-3.5"></i>
                        <span id="pause-text" data-template-id="pause-text">Pause</span>
                        <span id="resume-text" data-template-id="resume-text" class="hidden">Resume</span>
                    </button>

                    <!-- Light / Dark Theme Switcher Button -->
                    <button id="theme-toggle" data-template-id="theme-toggle" type="button" aria-pressed="false"
                        class="flex items-center gap-2 px-3 py-1.5 rounded-xl text-xs font-semibold bg-white dark:bg-slate-800 hover:bg-slate-100 dark:hover:bg-slate-700 border border-slate-300 dark:border-slate-700 text-slate-800 dark:text-slate-100 shadow-sm transition">
                        <i id="theme-icon-sun" data-lucide="sun" class="w-3.5 h-3.5 text-amber-500 hidden"></i>
                        <i id="theme-icon-moon" data-lucide="moon" class="w-3.5 h-3.5 text-indigo-500"></i>
                        <span id="light-theme-text" data-template-id="light-theme-text">Light Mode</span>
                        <span id="dark-theme-text" data-template-id="dark-theme-text" class="hidden">Dark Mode</span>
                    </button>
                </div>
            </div>
        </header>

        <!-- Main Workspace Container -->
        <main class="flex-1 max-w-7xl w-full mx-auto px-4 sm:px-6 lg:px-8 py-6 sm:py-8 space-y-6">

            <!-- Title & Hero Info Section -->
            <section class="space-y-4">
                <div class="flex flex-col md:flex-row md:items-center justify-between gap-4 p-5 rounded-2xl bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm">
                    <div class="flex items-center gap-4 sm:gap-5">
                        <div class="relative shrink-0">
                            <img src="sems-logo.jpg" alt="SEMS Emblem" class="w-16 h-16 sm:w-20 sm:h-20 rounded-2xl object-cover border-2 border-emerald-500/30 shadow-md shadow-emerald-500/10 bg-white dark:bg-slate-800">
                            <span class="absolute -bottom-1 -right-1 px-1.5 py-0.5 text-[9px] font-black uppercase tracking-wider rounded bg-emerald-600 text-white shadow-sm">
                                IoT
                            </span>
                        </div>
                        <div>
                            <div class="flex items-center gap-2 mb-1 flex-wrap">
                                <span data-template-id="eyebrow" class="text-xs font-bold uppercase tracking-widest text-emerald-600 dark:text-emerald-400 mono">
                                    ESP32 Telemetry &bull; Industrial Protocol
                                </span>
                                <span id="wifi-rssi-badge" class="inline-flex items-center px-2 py-0.5 rounded text-[11px] font-medium bg-slate-100 dark:bg-slate-800 text-slate-700 dark:text-slate-300 border border-slate-200 dark:border-slate-700">
                                    Wi-Fi: <span id="wifi-rssi-val">-62 dBm (Active)</span>
                                </span>
                            </div>
                            <h1 data-template-id="page-title" class="text-xl sm:text-2xl lg:text-3xl font-extrabold text-slate-900 dark:text-white tracking-tight">
                                Smart Environmental Monitoring System
                            </h1>
                            <p data-template-id="page-description" class="mt-1 text-xs sm:text-sm text-slate-600 dark:text-slate-300 max-w-2xl leading-relaxed">
                                Continuous multi-sensor telemetry for DHT22 climate tracking and MQ-135 air contamination profiling with autonomous Telegram alert dispatching.
                            </p>
                        </div>
                    </div>

                    <!-- Quick Action Presets / CSV Export -->
                    <div class="flex items-center gap-2 shrink-0 self-end md:self-center">
                        <button id="export-csv-btn" type="button"
                            class="inline-flex items-center gap-2 px-3.5 py-2 rounded-xl text-xs font-semibold bg-slate-50 dark:bg-slate-800 text-slate-700 dark:text-slate-200 border border-slate-300 dark:border-slate-700 hover:bg-slate-100 dark:hover:bg-slate-700 shadow-sm transition">
                            <i data-lucide="download" class="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400"></i>
                            Export Telemetry (.CSV)
                        </button>
                    </div>
                </div>
            </section>

            <!-- 4 Key Metric Cards (Temperature, Humidity, Air Quality, Incident Gateway) -->
            <section class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4 sm:gap-5">
                
                <!-- Metric 1: Temperature (DHT22) - colour fills by temperature level -->
                <article id="temp-card" data-template-id="temp-card" class="relative overflow-hidden rounded-2xl p-5 border-2 shadow-sm hover:shadow-md transition-colors duration-500 bg-emerald-100 border-emerald-400 dark:bg-emerald-900/50 dark:border-emerald-600">
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <span class="p-2 rounded-xl bg-white/70 dark:bg-slate-900/50 text-slate-700 dark:text-slate-200 border border-white/70 dark:border-slate-700/60">
                                <i data-lucide="thermometer" class="w-5 h-5"></i>
                            </span>
                            <div>
                                <h3 data-template-id="temp-label" class="text-sm font-semibold text-slate-900 dark:text-slate-100">Temperature</h3>
                                <p data-template-id="temp-source" class="text-[11px] font-medium text-slate-600 dark:text-slate-300">DHT22 Digital &bull; GPIO 26</p>
                            </div>
                        </div>
                        <span id="temp-status-badge" class="px-2 py-0.5 rounded-full text-[11px] font-bold bg-emerald-600 text-white">
                            Normal
                        </span>
                    </div>

                    <div class="mt-4 flex items-baseline gap-2">
                        <span id="temp-value" class="text-4xl font-extrabold text-slate-900 dark:text-white tracking-tight mono">26.4</span>
                        <span data-template-id="temp-unit" class="text-lg font-bold text-slate-700 dark:text-slate-200 mono">&deg;C</span>
                        <span id="temp-f-value" class="ml-auto text-xs font-semibold text-slate-600 dark:text-slate-300 mono">79.5 &deg;F</span>
                    </div>

                    <div class="mt-4 pt-3 border-t border-slate-900/10 dark:border-white/10 flex items-center justify-between text-xs text-slate-600 dark:text-slate-300">
                        <span>Threshold: <strong id="temp-stat-limit" class="text-slate-800 dark:text-slate-100 mono">30.0&deg;C</strong></span>
                        <span>Avg: <strong id="temp-stat-avg" class="text-slate-800 dark:text-slate-100 mono">26.1&deg;C</strong></span>
                    </div>
                </article>

                <!-- Metric 2: Relative Humidity (DHT22) -->
                <article data-template-id="humidity-card" class="relative overflow-hidden rounded-2xl p-5 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm hover:shadow-md transition">
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <span class="p-2 rounded-xl bg-sky-50 dark:bg-sky-950/50 text-sky-600 dark:text-sky-400 border border-sky-200/60 dark:border-sky-800/40">
                                <i data-lucide="droplets" class="w-5 h-5"></i>
                            </span>
                            <div>
                                <h3 data-template-id="humidity-label" class="text-sm font-semibold text-slate-900 dark:text-slate-100">Relative Humidity</h3>
                                <p data-template-id="humidity-source" class="text-[11px] font-medium text-slate-500 dark:text-slate-400">DHT22 Digital &bull; GPIO 4</p>
                            </div>
                        </div>
                        <span id="humidity-status-badge" class="px-2 py-0.5 rounded-full text-[11px] font-bold bg-sky-100 text-sky-800 dark:bg-sky-950/80 dark:text-sky-300">
                            Optimal
                        </span>
                    </div>

                    <div class="mt-4 flex items-baseline gap-2">
                        <span id="humidity-value" class="text-4xl font-extrabold text-slate-900 dark:text-white tracking-tight mono">58</span>
                        <span data-template-id="humidity-unit" class="text-lg font-bold text-sky-600 dark:text-sky-400 mono">% RH</span>
                    </div>

                    <div class="mt-4 pt-3 border-t border-slate-100 dark:border-slate-800/80 flex items-center justify-between text-xs text-slate-500 dark:text-slate-400">
                        <span>Dew Point: <strong id="dew-point-value" class="text-slate-700 dark:text-slate-300 mono">17.4&deg;C</strong></span>
                        <span>Comfort: <strong class="text-emerald-600 dark:text-emerald-400">Balanced</strong></span>
                    </div>
                </article>

                <!-- Metric 3: Air Quality & Gas Signal (MQ-135) - colour fills by air quality rating -->
                <article id="air-card" data-template-id="air-card" class="relative overflow-hidden rounded-2xl p-5 border-2 shadow-sm hover:shadow-md transition-colors duration-500 bg-emerald-100 border-emerald-400 dark:bg-emerald-900/50 dark:border-emerald-600">
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <span class="p-2 rounded-xl bg-white/70 dark:bg-slate-900/50 text-slate-700 dark:text-slate-200 border border-white/70 dark:border-slate-700/60">
                                <i data-lucide="wind" class="w-5 h-5"></i>
                            </span>
                            <div>
                                <h3 data-template-id="air-label" class="text-sm font-semibold text-slate-900 dark:text-slate-100">Air Quality Signal</h3>
                                <p data-template-id="air-source" class="text-[11px] font-medium text-slate-600 dark:text-slate-300">MQ-135 Gas Sensor &bull; GPIO 36 (VP)</p>
                            </div>
                        </div>
                        <span id="air-status-badge" class="px-2 py-0.5 rounded-full text-[11px] font-bold bg-emerald-600 text-white">
                            GOOD
                        </span>
                    </div>

                    <div class="mt-4 flex items-baseline gap-2">
                        <span id="air-value" class="text-4xl font-extrabold text-slate-900 dark:text-white tracking-tight mono">470</span>
                        <span data-template-id="air-unit" class="text-lg font-bold text-slate-700 dark:text-slate-200 mono">ppm</span>
                        <span class="ml-auto text-xs font-semibold text-slate-600 dark:text-slate-300 mono">Raw: <span id="air-raw-value">410</span></span>
                    </div>

                    <div class="mt-4 pt-3 border-t border-slate-900/10 dark:border-white/10 flex items-center justify-between text-xs text-slate-600 dark:text-slate-300">
                        <span>Limit: <strong id="air-stat-limit" class="text-slate-800 dark:text-slate-100 mono">1000 ppm</strong></span>
                        <span>Rating: <strong id="air-rating-text" class="font-bold text-emerald-800 dark:text-emerald-300">Good Air Quality</strong></span>
                    </div>
                </article>

                <!-- Metric 4: Telegram Gateway & Node Health -->
                <article class="relative overflow-hidden rounded-2xl p-5 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm hover:shadow-md transition">
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <span class="p-2 rounded-xl bg-indigo-50 dark:bg-indigo-950/50 text-indigo-600 dark:text-indigo-400 border border-indigo-200/60 dark:border-indigo-800/40">
                                <i data-lucide="send" class="w-5 h-5"></i>
                            </span>
                            <div>
                                <h3 class="text-sm font-semibold text-slate-900 dark:text-slate-100">Telegram Bot Gateway</h3>
                                <p class="text-[11px] font-medium text-slate-500 dark:text-slate-400">HTTPS SSL Webhook Pipeline</p>
                            </div>
                        </div>
                        <span class="px-2 py-0.5 rounded-full text-[11px] font-bold bg-indigo-100 text-indigo-800 dark:bg-indigo-950/80 dark:text-indigo-300">
                            Armed
                        </span>
                    </div>

                    <div class="mt-4 flex items-baseline gap-2">
                        <span id="alert-counter" class="text-4xl font-extrabold text-slate-900 dark:text-white tracking-tight mono">0</span>
                        <span class="text-sm font-bold text-indigo-600 dark:text-indigo-400">Active Incidents</span>
                    </div>

                    <div class="mt-4 pt-3 border-t border-slate-100 dark:border-slate-800/80 flex items-center justify-between text-xs text-slate-500 dark:text-slate-400">
                        <span>Packets: <strong id="packet-counter" class="text-slate-700 dark:text-slate-300 mono">124</strong></span>
                        <span>Uptime: <strong id="node-uptime" class="text-slate-700 dark:text-slate-300 mono">00:06:12</strong></span>
                    </div>
                </article>

            </section>

            <!-- Interactive Visual Graphs Section -->
            <section class="grid grid-cols-1 lg:grid-cols-2 gap-6">
                
                <!-- Chart 1: Dual-Axis Climate Telemetry (Temperature & Humidity) -->
                <article data-template-id="climate-chart-panel" class="rounded-2xl p-5 sm:p-6 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm flex flex-col justify-between">
                    <div>
                        <div class="flex flex-wrap items-center justify-between gap-3 mb-2">
                            <div>
                                <h2 id="climate-heading" data-template-id="climate-chart-heading" class="text-lg font-bold text-slate-900 dark:text-white flex items-center gap-2">
                                    <i data-lucide="activity" class="w-4 h-4 text-amber-500"></i>
                                    Temperature &amp; Humidity Trend
                                </h2>
                                <p data-template-id="climate-chart-description" class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                                    Simultaneous dual-axis telemetry curve (DHT22 Sensor)
                                </p>
                            </div>
                            <!-- Legend pills -->
                            <div class="flex items-center gap-3 text-xs mono">
                                <span class="inline-flex items-center gap-1.5 text-amber-600 dark:text-amber-400 font-medium">
                                    <span class="w-2.5 h-2.5 rounded-full bg-amber-500"></span> Temp (&deg;C)
                                </span>
                                <span class="inline-flex items-center gap-1.5 text-sky-600 dark:text-sky-400 font-medium">
                                    <span class="w-2.5 h-2.5 rounded-full bg-sky-500"></span> Humidity (%)
                                </span>
                            </div>
                        </div>
                    </div>

                    <!-- Chart Canvas Box with fixed clear aspect -->
                    <div class="relative w-full h-64 sm:h-72 mt-4">
                        <canvas id="climate-chart" role="img" aria-label="Real-time chart of DHT22 temperature and humidity history"></canvas>
                    </div>

                    <div class="mt-3 pt-3 border-t border-slate-100 dark:border-slate-800/80 flex items-center justify-between text-xs text-slate-500 dark:text-slate-400">
                        <span>Left Axis: <strong class="text-amber-600 dark:text-amber-400">Temperature (16&ndash;42 &deg;C)</strong></span>
                        <span>Right Axis: <strong class="text-sky-600 dark:text-sky-400">Humidity (20&ndash;100 %)</strong></span>
                    </div>
                </article>

                <!-- Chart 2: MQ-135 Gas & Air Quality Telemetry -->
                <article data-template-id="air-chart-panel" class="rounded-2xl p-5 sm:p-6 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm flex flex-col justify-between">
                    <div>
                        <div class="flex flex-wrap items-center justify-between gap-3 mb-2">
                            <div>
                                <h2 id="air-heading" data-template-id="air-chart-heading" class="text-lg font-bold text-slate-900 dark:text-white flex items-center gap-2">
                                    <i data-lucide="gauge" class="w-4 h-4 text-emerald-500"></i>
                                    Air-Quality Contamination Signal
                                </h2>
                                <p data-template-id="air-chart-description" class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                                    Gas level (ppm) with raw sensor value &bull; Real-time vs configured ppm limit
                                </p>
                            </div>
                            <!-- Threshold indicator pill -->
                            <div class="flex items-center gap-3 text-xs mono">
                                <span class="inline-flex items-center gap-1.5 text-emerald-600 dark:text-emerald-400 font-medium">
                                    <span class="w-2.5 h-2.5 rounded-full bg-emerald-500"></span> Gas ppm
                                </span>
                                <span class="inline-flex items-center gap-1.5 text-slate-500 dark:text-slate-400 font-medium">
                                    <span class="w-2.5 h-2.5 rounded-full bg-slate-400"></span> Raw
                                </span>
                                <span class="inline-flex items-center gap-1.5 text-rose-500 font-medium">
                                    <span class="w-3 h-0.5 bg-rose-500 border-t border-dashed"></span> Trigger Limit
                                </span>
                            </div>
                        </div>
                    </div>

                    <!-- Chart Canvas Box -->
                    <div class="relative w-full h-64 sm:h-72 mt-4">
                        <canvas id="air-chart" role="img" aria-label="Real-time chart of MQ135 gas ppm and raw sensor value history"></canvas>
                    </div>

                    <p data-template-id="air-chart-note" class="mt-3 pt-3 border-t border-slate-100 dark:border-slate-800/80 text-xs text-slate-500 dark:text-slate-400 leading-normal">
                        MQ-135 detects NH3, NOx, Alcohol, Benzene, Smoke, and CO2. Requires 24h heater pre-heating for calibrated ppm conversion.
                    </p>
                </article>

            </section>

            <!-- Configuration & Control Center + Live Incident Stream -->
            <section class="grid grid-cols-1 lg:grid-cols-12 gap-6">
                
                <!-- Left: Alert Thresholds & Simulation Controls (7 Cols) -->
                <div class="lg:col-span-7 space-y-6">
                    <section data-template-id="controls-panel" class="rounded-2xl p-5 sm:p-7 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm">
                        <div class="flex items-center justify-between pb-4 border-b border-slate-100 dark:border-slate-800">
                            <div>
                                <h2 id="controls-heading" data-template-id="controls-heading" class="text-lg font-bold text-slate-900 dark:text-white flex items-center gap-2">
                                    <i data-lucide="sliders" class="w-5 h-5 text-emerald-600 dark:text-emerald-400"></i>
                                    Incident Threshold Controls
                                </h2>
                                <p data-template-id="controls-description" class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                                    Configure autonomous trigger limits for local alarms and Telegram notifications.
                                </p>
                            </div>
                            <span class="text-xs px-2.5 py-1 rounded-lg bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-300 font-medium">
                                ESP32 Memory: Active
                            </span>
                        </div>

                        <!-- Threshold Configuration Form -->
                        <form id="threshold-form" class="mt-6 space-y-5">
                            
                            <!-- Temperature Threshold Group -->
                            <div class="space-y-2">
                                <div class="flex items-center justify-between">
                                    <label for="temp-threshold" data-template-id="temp-threshold-label" class="text-sm font-semibold text-slate-800 dark:text-slate-200">
                                        Temperature Limit (&deg;C)
                                    </label>
                                    <span class="text-xs font-bold text-amber-600 dark:text-amber-400 mono">
                                        Warning above <span id="temp-threshold-display">30.0</span> &deg;C
                                    </span>
                                </div>
                                <div class="grid grid-cols-12 gap-3 items-center">
                                    <input id="temp-threshold-slider" type="range" min="18" max="45" step="0.5" value="30"
                                        class="col-span-8 sm:col-span-9 h-2 bg-slate-200 dark:bg-slate-700 rounded-lg appearance-none cursor-pointer accent-amber-500">
                                    <div class="col-span-4 sm:col-span-3">
                                        <div class="relative">
                                            <input id="temp-threshold" type="number" min="-20" max="80" step="0.5" value="30" required
                                                class="w-full pl-3 pr-8 py-2 rounded-xl text-sm font-semibold mono bg-slate-50 dark:bg-slate-800 text-slate-900 dark:text-white border border-slate-300 dark:border-slate-700 focus:ring-2 focus:ring-emerald-500 focus:outline-none transition">
                                            <span class="absolute right-3 top-2.5 text-xs text-slate-400 mono">&deg;C</span>
                                        </div>
                                    </div>
                                </div>
                            </div>

                            <!-- Air Quality Threshold Group (MQ-135 gas concentration, ppm) -->
                            <div class="space-y-2">
                                <div class="flex items-center justify-between">
                                    <label for="air-threshold" data-template-id="air-threshold-label" class="text-sm font-semibold text-slate-800 dark:text-slate-200">
                                        MQ-135 Gas Limit (ppm)
                                    </label>
                                    <span class="text-xs font-bold text-emerald-600 dark:text-emerald-400 mono">
                                        Warning above <span id="air-threshold-display">1000</span> ppm
                                    </span>
                                </div>
                                <div class="grid grid-cols-12 gap-3 items-center">
                                    <input id="air-threshold-slider" type="range" min="350" max="5000" step="25" value="1000"
                                        class="col-span-8 sm:col-span-9 h-2 bg-slate-200 dark:bg-slate-700 rounded-lg appearance-none cursor-pointer accent-emerald-500">
                                    <div class="col-span-4 sm:col-span-3">
                                        <div class="relative">
                                            <input id="air-threshold" type="number" min="0" max="5000" step="1" value="1000" required
                                                class="w-full pl-3 pr-10 py-2 rounded-xl text-sm font-semibold mono bg-slate-50 dark:bg-slate-800 text-slate-900 dark:text-white border border-slate-300 dark:border-slate-700 focus:ring-2 focus:ring-emerald-500 focus:outline-none transition">
                                            <span class="absolute right-3 top-2.5 text-xs text-slate-400 mono">ppm</span>
                                        </div>
                                    </div>
                                </div>
                            </div>

                            <!-- Preset Environment Configurations -->
                            <div class="pt-2 space-y-2.5">
                                <span class="block text-xs font-semibold uppercase tracking-wider text-slate-400">Preset Environment Profiles</span>
                                <div class="grid grid-cols-2 sm:grid-cols-3 gap-2">
                                    <button type="button" data-preset="construction" class="preset-btn px-2.5 py-2 rounded-lg text-xs font-medium bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 border border-slate-200 dark:border-slate-700 transition">
                                        Construction Site
                                    </button>
                                    <button type="button" data-preset="factory" class="preset-btn px-2.5 py-2 rounded-lg text-xs font-medium bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 border border-slate-200 dark:border-slate-700 transition">
                                        Factory
                                    </button>
                                    <button type="button" data-preset="warehouse" class="preset-btn px-2.5 py-2 rounded-lg text-xs font-medium bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 border border-slate-200 dark:border-slate-700 transition">
                                        Warehouse
                                    </button>
                                    <button type="button" data-preset="lab" class="preset-btn px-2.5 py-2 rounded-lg text-xs font-medium bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 border border-slate-200 dark:border-slate-700 transition">
                                        Laboratory
                                    </button>
                                    <button type="button" data-preset="others" class="preset-btn col-span-2 sm:col-span-2 px-2.5 py-2 rounded-lg text-xs font-medium bg-slate-100 hover:bg-slate-200/80 dark:bg-slate-800 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 border border-slate-200 dark:border-slate-700 transition">
                                        Others (Specify Location...)
                                    </button>
                                </div>
                                <div id="custom-env-box" class="hidden pt-1">
                                    <label for="custom-env-input" class="block text-[11px] font-semibold text-slate-600 dark:text-slate-300 mb-1">
                                        Specify Deployment Location / Environment
                                    </label>
                                    <input id="custom-env-input" type="text" maxlength="60" placeholder="e.g., Underground Car Park, Kitchen, Workshop..."
                                        class="w-full px-3 py-2 rounded-xl text-xs font-medium bg-slate-50 dark:bg-slate-800 text-slate-900 dark:text-white border border-slate-300 dark:border-slate-700 focus:ring-2 focus:ring-emerald-500 focus:outline-none transition">
                                </div>
                            </div>

                            <!-- Actions Row -->
                            <div class="pt-2 flex flex-wrap sm:flex-nowrap items-center gap-3">
                                <button data-template-id="save-thresholds" type="submit"
                                    class="w-full sm:w-auto flex-1 inline-flex items-center justify-center gap-2 py-2.5 px-4 rounded-xl font-bold text-sm bg-emerald-600 hover:bg-emerald-500 text-white shadow-md shadow-emerald-600/20 transition">
                                    <i data-lucide="check" class="w-4 h-4"></i>
                                    Apply Thresholds
                                </button>
                                <button id="test-alert" data-template-id="test-alert" type="button"
                                    class="w-full sm:w-auto inline-flex items-center justify-center gap-2 py-2.5 px-4 rounded-xl font-bold text-sm bg-amber-50 hover:bg-amber-100 text-amber-800 dark:bg-amber-950/50 dark:hover:bg-amber-900/60 dark:text-amber-200 border border-amber-300 dark:border-amber-800 transition">
                                    <i data-lucide="bell" class="w-4 h-4"></i>
                                    Test Telegram Alert
                                </button>
                                <button id="spike-temp-btn" type="button" title="Spike temperature to trigger alert"
                                    class="p-2.5 rounded-xl text-slate-600 dark:text-slate-300 bg-slate-100 hover:bg-slate-200 dark:bg-slate-800 dark:hover:bg-slate-700 border border-slate-200 dark:border-slate-700 transition">
                                    <i data-lucide="flame" class="w-4 h-4 text-orange-500"></i>
                                </button>
                            </div>

                            <!-- Dedicated Demo Simulation Trigger -->
                            <div class="pt-2 border-t border-slate-200/80 dark:border-slate-800 flex flex-col sm:flex-row sm:items-center justify-between gap-3">
                                <div>
                                    <p class="text-xs font-bold text-slate-800 dark:text-slate-200">Presentation &amp; Demo Mode</p>
                                    <p class="text-[11px] text-slate-500 dark:text-slate-400">Injects a simulated sensor spike on ESP32, graphs, OLED &amp; Telegram bot without affecting real calibration.</p>
                                </div>
                                <button id="demo-sim-btn" type="button"
                                    class="shrink-0 inline-flex items-center justify-center gap-2 py-2.5 px-4 rounded-xl font-bold text-xs bg-violet-600 hover:bg-violet-500 text-white shadow-md shadow-violet-600/20 transition">
                                    <i data-lucide="flask-conical" class="w-4 h-4"></i>
                                    <span id="demo-sim-btn-text">Trigger Demo Simulation</span>
                                </button>
                            </div>

                            <p id="feedback" role="status" aria-live="polite" class="min-h-5 text-xs font-medium text-emerald-600 dark:text-emerald-400"></p>
                        </form>
                    </section>

                    <!-- Real-Time Telegram Message Preview Card -->
                    <div class="rounded-2xl p-5 bg-gradient-to-br from-slate-900 to-slate-950 text-white border border-slate-800 shadow-md">
                        <div class="flex items-center justify-between pb-3 border-b border-slate-800">
                            <div class="flex items-center gap-2.5">
                                <img src="sems-logo.jpg" alt="SEMS Alert Bot" class="w-8 h-8 rounded-full object-cover border border-[#2AABEE] shadow-sm">
                                <div>
                                    <div class="flex items-center gap-1.5">
                                        <span class="text-sm font-bold">SEMS_BotSys</span>
                                        <i data-lucide="badge-check" class="w-4 h-4 text-[#2AABEE]"></i>
                                    </div>
                                    <span class="text-[11px] text-slate-400">@SemibotSys_Bot &bull; Telegram Notification Channel</span>
                                </div>
                            </div>
                            <a href="https://t.me/SemibotSys_Bot" target="_blank" rel="noopener"
                                class="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-lg bg-[#2AABEE] hover:bg-[#229ed9] text-white text-xs font-semibold shadow transition">
                                <i data-lucide="send" class="w-3.5 h-3.5"></i>
                                Open Bot
                            </a>
                        </div>

                        <!-- Active Bot Credentials & Pairing Helper -->
                        <div class="mt-3 p-2.5 rounded-xl bg-slate-800/50 border border-slate-700/50 flex flex-wrap items-center justify-between gap-2 text-[11px] mono">
                            <div>
                                <span class="text-slate-400">Token ID:</span>
                                <code class="text-emerald-400 font-bold ml-1">9063237995:AAFZgh...FHGxc</code>
                            </div>
                            <span id="tg-link-status" class="text-xs text-sky-400">Send <strong>/start</strong> to link</span>
                        </div>

                        <!-- Simulated Telegram Message Body -->
                        <div class="mt-3 p-3.5 rounded-xl bg-slate-800/80 border border-slate-700/60 font-mono text-xs space-y-1.5 leading-relaxed text-slate-200">
                            <p class="font-bold text-amber-400 flex items-center gap-1.5">
                                <span>&#9888;&#65039;</span> <span id="tg-preview-title">SEMS TELEMETRY WARNING</span>
                            </p>
                            <p class="text-slate-300">
                                <strong>Node:</strong> ESP32-NODE-01 (IP: <span id="tg-preview-ip">192.168.1.105</span>)<br>
                                <strong>Condition:</strong> <span id="tg-preview-condition">MQ-135 Gas Reading above limit</span><br>
                                <strong>Current:</strong> <span id="tg-preview-val" class="text-emerald-400 font-bold">470 ppm (Raw 410)</span> (Limit: <span id="tg-preview-limit">1000 ppm</span>)<br>
                                <strong>Timestamp:</strong> <span id="tg-preview-time">14:24:00</span>
                            </p>
                            <p class="text-[11px] text-slate-400 pt-1 border-t border-slate-700">
                                Channel: Autonomous dispatch via @SemibotSys_Bot HTTPS Webhook.
                            </p>
                        </div>
                    </div>
                </div>

                <!-- Right: Incident & Activity Log Feed (5 Cols) -->
                <div class="lg:col-span-5">
                    <section data-template-id="activity-panel" class="h-full rounded-2xl p-5 sm:p-7 bg-white dark:bg-slate-900 border border-slate-200/90 dark:border-slate-800 shadow-sm flex flex-col justify-between">
                        <div>
                            <div class="flex items-center justify-between pb-4 border-b border-slate-100 dark:border-slate-800">
                                <div>
                                    <h2 id="activity-heading" data-template-id="activity-heading" class="text-lg font-bold text-slate-900 dark:text-white flex items-center gap-2">
                                        <i data-lucide="shield-alert" class="w-5 h-5 text-indigo-600 dark:text-indigo-400"></i>
                                        Incident &amp; Audit Log
                                    </h2>
                                    <p data-template-id="activity-description" class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                                        Local telemetry triggers &amp; Telegram bot dispatch logs
                                    </p>
                                </div>
                                <button id="clear-activity-btn" type="button" title="Clear event stream"
                                    class="text-xs px-2.5 py-1 rounded-lg text-slate-500 hover:text-slate-800 dark:text-slate-400 dark:hover:text-slate-200 bg-slate-100 dark:bg-slate-800 transition">
                                    Clear
                                </button>
                            </div>

                            <!-- Filter tabs -->
                            <div class="flex items-center gap-2 mt-4 text-xs">
                                <button id="log-filter-all" class="px-3 py-1 rounded-full bg-slate-900 text-white dark:bg-slate-100 dark:text-slate-900 font-semibold">
                                    All Logs
                                </button>
                                <button id="log-filter-alerts" class="px-3 py-1 rounded-full bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-300 hover:bg-slate-200 font-medium">
                                    Alerts Only
                                </button>
                            </div>

                            <!-- Scrollable activity log container -->
                            <div class="mt-4 custom-scrollbar overflow-y-auto max-h-[460px] pr-1">
                                <ol id="activity-list" class="space-y-3" aria-live="polite">
                                    <li id="activity-empty" data-template-id="activity-empty"
                                        class="rounded-xl border border-dashed border-slate-300 dark:border-slate-700 p-5 text-center text-xs text-slate-500 dark:text-slate-400">
                                        <i data-lucide="check-circle-2" class="w-6 h-6 mx-auto mb-2 text-emerald-500"></i>
                                        System operating within nominal thresholds. No alerts logged yet.
                                    </li>
                                </ol>
                            </div>
                        </div>

                        <div class="mt-4 pt-3 border-t border-slate-100 dark:border-slate-800 text-[11px] text-slate-400 flex items-center justify-between">
                            <span>Telegram Webhook: <strong>Active</strong></span>
                            <span>Cooldown: <strong>15s suppression</strong></span>
                        </div>
                    </section>
                </div>

            </section>

        </main>

        <!-- Standard Professional Footer -->
        <footer data-template-id="site-footer" class="border-t border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 transition-colors mt-auto">
            <div class="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-5 flex flex-wrap items-center justify-between gap-4 text-xs text-slate-500 dark:text-slate-400">
                <div class="flex items-center gap-2.5">
                    <img src="sems-logo.jpg" alt="SEMS Logo" class="w-5 h-5 rounded-md object-cover border border-slate-300 dark:border-slate-700">
                    <span class="font-bold text-slate-800 dark:text-slate-200">SEMS v2.4</span>
                    <span>&bull;</span>
                    <p data-template-id="footer-text" class="inline">Smart Environmental Monitoring System &bull; ESP32 IoT Architecture</p>
                </div>
                <div class="flex items-center gap-4 mono text-[11px]">
                    <span class="inline-flex items-center gap-1.5">
                        <span class="w-2 h-2 rounded-full bg-emerald-500"></span> DHT22: Online
                    </span>
                    <span class="inline-flex items-center gap-1.5">
                        <span class="w-2 h-2 rounded-full bg-emerald-500"></span> MQ-135: Online
                    </span>
                    <span class="inline-flex items-center gap-1.5">
                        <span class="w-2 h-2 rounded-full bg-indigo-500"></span> Telegram: Ready
                    </span>
                </div>
            </div>
        </footer>

    </div>

    <!-- Application Telemetry Engine & Reactive Chart Logic -->
    <script>
        (() => {
            // State Model
            const values = {
                temperature: 26.4,
                humidity: 58,
                air: 410,
                airPpm: 470,
                packets: 124,
                activeIncidents: 0
            };

            const limits = {
                temperature: 30.0,
                air: 1000
            };

            const stats = {
                tempSum: 26.4,
                tempCount: 1,
                tempMin: 26.4,
                tempMax: 26.4
            };

            const history = {
                labels: [],
                temperature: [],
                humidity: [],
                air: [],
                airPpm: [],
                airThreshold: []
            };

            // MQ-135 conversion (same formula as Esp32GasTest.ino / SEMS_ESP32.ino) - used for offline preview
            const MQ135 = { RL: 10.0, R0: 76.63, A: 110.47, B: -2.862, BASE: 400, VREF: 3.3 };
            function mq135PpmFromRaw(raw) {
                const v = (raw / 4095) * MQ135.VREF;
                if (v <= 0.01) return MQ135.BASE;
                const rs = MQ135.RL * (MQ135.VREF - v) / v;
                const ratio = rs / MQ135.R0;
                if (ratio <= 0) return MQ135.BASE;
                const ppm = MQ135.A * Math.pow(ratio, MQ135.B) + MQ135.BASE;
                return Math.round(Math.min(5000, Math.max(350, ppm)));
            }

            // Air quality rating (GOOD < 600 ppm, MODERATE < 1000, POOR < 2000, else HAZARDOUS)
            function airQualityLevel(ppm) {
                if (ppm < 600) return 'GOOD';
                if (ppm < 1000) return 'MODERATE';
                if (ppm < 2000) return 'POOR';
                return 'HAZARDOUS';
            }
            const AIR_CARD_BASE = 'relative overflow-hidden rounded-2xl p-5 border-2 shadow-sm hover:shadow-md transition-colors duration-500 ';
            const AIR_QUALITY_STYLES = {
                GOOD: {
                    label: 'Good Air Quality',
                    card: 'bg-emerald-100 border-emerald-400 dark:bg-emerald-900/50 dark:border-emerald-600',
                    badge: 'bg-emerald-600 text-white',
                    text: 'text-emerald-800 dark:text-emerald-300',
                    line: '#10b981', fill: 'rgba(16, 185, 129, 0.15)'
                },
                MODERATE: {
                    label: 'Moderate Air Quality',
                    card: 'bg-orange-100 border-orange-400 dark:bg-orange-900/50 dark:border-orange-600',
                    badge: 'bg-orange-500 text-white',
                    text: 'text-orange-800 dark:text-orange-300',
                    line: '#f97316', fill: 'rgba(249, 115, 22, 0.15)'
                },
                POOR: {
                    label: 'Poor Air Quality',
                    card: 'bg-yellow-100 border-yellow-400 dark:bg-yellow-900/40 dark:border-yellow-500',
                    badge: 'bg-yellow-400 text-yellow-950',
                    text: 'text-yellow-800 dark:text-yellow-300',
                    line: '#eab308', fill: 'rgba(234, 179, 8, 0.18)'
                },
                HAZARDOUS: {
                    label: 'Hazardous Air Quality',
                    card: 'bg-red-100 border-red-500 dark:bg-red-950/70 dark:border-red-600',
                    badge: 'bg-red-600 text-white',
                    text: 'text-red-700 dark:text-red-300',
                    line: '#dc2626', fill: 'rgba(220, 38, 38, 0.15)'
                }
            };

            const startTime = Date.now();
            let running = true;
            let audioMuted = false;
            let alertFilter = 'all';
            const lastAlert = { temperature: 0, air: 0 };

            // Helper Utilities
            const clamp = (v, min, max) => Math.min(max, Math.max(min, v));
            const fmtTime = (d = new Date(), withSec = true) =>
                d.toLocaleTimeString([], withSec
                    ? { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' }
                    : { hour12: false, hour: '2-digit', minute: '2-digit' });
            const clock = () => fmtTime(new Date(), true);
            const cToF = c => ((c * 9/5) + 32).toFixed(1);
            
            // Calculate approximate Dew Point using Magnus formula
            function calculateDewPoint(tempC, rh) {
                const a = 17.27;
                const b = 237.7;
                const alpha = ((a * tempC) / (b + tempC)) + Math.log(rh / 100.0);
                return ((b * alpha) / (a - alpha)).toFixed(1);
            }

            // Web Audio Synthetic Chime (no external asset needed)
            function playAlertChime() {
                if (audioMuted) return;
                try {
                    const ctx = new (window.AudioContext || window.webkitAudioContext)();
                    const osc = ctx.createOscillator();
                    const gain = ctx.createGain();
                    osc.type = 'sine';
                    osc.frequency.setValueAtTime(880, ctx.currentTime);
                    osc.frequency.exponentialRampToValueAtTime(440, ctx.currentTime + 0.25);
                    gain.gain.setValueAtTime(0.15, ctx.currentTime);
                    gain.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.25);
                    osc.connect(gain);
                    gain.connect(ctx.destination);
                    osc.start();
                    osc.stop(ctx.currentTime + 0.26);
                } catch (e) {
                    // Audio context not initialized until user gesture
                }
            }

            // DOM Elements
            const list = document.getElementById('activity-list');
            const empty = document.getElementById('activity-empty');
            const feedback = document.getElementById('feedback');
            const liveClockEl = document.getElementById('live-clock');
            const uptimeEl = document.getElementById('node-uptime');
            const packetCounterEl = document.getElementById('packet-counter');
            const alertCounterEl = document.getElementById('alert-counter');

            const themeToggleBtn = document.getElementById('theme-toggle');
            const lightThemeText = document.getElementById('light-theme-text');
            const darkThemeText = document.getElementById('dark-theme-text');
            const themeIconSun = document.getElementById('theme-icon-sun');
            const themeIconMoon = document.getElementById('theme-icon-moon');

            // Theme Management with LocalStorage Support
            function isDarkMode() {
                return document.documentElement.classList.contains('dark') || document.body.classList.contains('dark-mode');
            }

            function getChartThemeColors(dark) {
                return {
                    ticks: dark ? '#94a3b8' : '#475569',
                    grid: dark ? 'rgba(255, 255, 255, 0.08)' : 'rgba(15, 23, 42, 0.08)',
                    legend: dark ? '#f1f5f9' : '#0f172a',
                    tooltipBg: dark ? '#1e293b' : '#ffffff',
                    tooltipText: dark ? '#f8fafc' : '#0f172a',
                    tooltipBorder: dark ? '#334155' : '#e2e8f0'
                };
            }

            function applyTheme(dark) {
                if (dark) {
                    document.documentElement.classList.add('dark');
                    document.body.classList.add('dark-mode');
                    themeToggleBtn.setAttribute('aria-pressed', 'true');
                    lightThemeText.classList.add('hidden');
                    darkThemeText.classList.remove('hidden');
                    themeIconSun.classList.remove('hidden');
                    themeIconMoon.classList.add('hidden');
                } else {
                    document.documentElement.classList.remove('dark');
                    document.body.classList.remove('dark-mode');
                    themeToggleBtn.setAttribute('aria-pressed', 'false');
                    lightThemeText.classList.remove('hidden');
                    darkThemeText.classList.add('hidden');
                    themeIconSun.classList.add('hidden');
                    themeIconMoon.classList.remove('hidden');
                }
                localStorage.setItem('sems_theme', dark ? 'dark' : 'light');
                updateChartThemes();
            }

            // Initial Theme Detection
            const savedTheme = localStorage.getItem('sems_theme');
            const prefersDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
            const initDark = savedTheme ? (savedTheme === 'dark') : prefersDark;

            // Chart 1: Dual-Axis Temperature and Humidity
            const climateCtx = document.getElementById('climate-chart').getContext('2d');
            const initialColors = getChartThemeColors(initDark);

            const climateChart = new Chart(climateCtx, {
                type: 'line',
                data: {
                    labels: history.labels,
                    datasets: [
                        {
                            label: 'Temperature (°C)',
                            data: history.temperature,
                            yAxisID: 'yTemp',
                            borderColor: '#f97316',
                            backgroundColor: 'rgba(249, 115, 22, 0.12)',
                            fill: true,
                            tension: 0.35,
                            borderWidth: 2.5,
                            pointRadius: 3,
                            pointBackgroundColor: '#f97316',
                            pointHoverRadius: 6
                        },
                        {
                            label: 'Humidity (%)',
                            data: history.humidity,
                            yAxisID: 'yHumidity',
                            borderColor: '#0ea5e9',
                            backgroundColor: 'rgba(14, 165, 233, 0.10)',
                            fill: true,
                            tension: 0.35,
                            borderWidth: 2.5,
                            pointRadius: 3,
                            pointBackgroundColor: '#0ea5e9',
                            pointHoverRadius: 6
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    animation: false,
                    interaction: { mode: 'index', intersect: false },
                    plugins: {
                        legend: { display: false },
                        tooltip: {
                            backgroundColor: initialColors.tooltipBg,
                            titleColor: initialColors.tooltipText,
                            bodyColor: initialColors.tooltipText,
                            borderColor: initialColors.tooltipBorder,
                            borderWidth: 1,
                            padding: 10,
                            boxPadding: 4,
                            usePointStyle: true,
                            titleFont: { family: 'Plus Jakarta Sans', weight: 'bold' },
                            bodyFont: { family: 'JetBrains Mono' }
                        }
                    },
                    scales: {
                        x: {
                            ticks: { color: initialColors.ticks, font: { family: 'JetBrains Mono', size: 10 }, maxTicksLimit: 6 },
                            grid: { color: initialColors.grid }
                        },
                        yTemp: {
                            type: 'linear',
                            position: 'left',
                            min: 16,
                            max: 42,
                            ticks: {
                                color: '#f97316',
                                font: { family: 'JetBrains Mono', size: 11 },
                                callback: v => v + '°C'
                            },
                            grid: { color: initialColors.grid }
                        },
                        yHumidity: {
                            type: 'linear',
                            position: 'right',
                            min: 20,
                            max: 100,
                            ticks: {
                                color: '#0ea5e9',
                                font: { family: 'JetBrains Mono', size: 11 },
                                callback: v => v + '%'
                            },
                            grid: { drawOnChartArea: false }
                        }
                    }
                }
            });

            // Chart 2: MQ-135 Gas Telemetry (ppm on left axis, raw value & raw limit on right axis)
            const airCtx = document.getElementById('air-chart').getContext('2d');
            const airChart = new Chart(airCtx, {
                type: 'line',
                data: {
                    labels: history.labels,
                    datasets: [
                        {
                            label: 'Gas Level (ppm)',
                            data: history.airPpm,
                            yAxisID: 'yPpm',
                            borderColor: '#10b981',
                            backgroundColor: 'rgba(16, 185, 129, 0.15)',
                            fill: true,
                            tension: 0.35,
                            borderWidth: 2.5,
                            pointRadius: 3,
                            pointBackgroundColor: '#10b981',
                            pointHoverRadius: 6
                        },
                        {
                            label: 'Raw Value',
                            data: history.air,
                            yAxisID: 'yRaw',
                            borderColor: '#94a3b8',
                            fill: false,
                            tension: 0.35,
                            borderWidth: 1.5,
                            pointRadius: 0
                        },
                        {
                            label: 'Trigger Limit (ppm)',
                            data: history.airThreshold,
                            yAxisID: 'yPpm',
                            borderColor: '#f43f5e',
                            borderDash: [5, 4],
                            fill: false,
                            tension: 0,
                            borderWidth: 1.8,
                            pointRadius: 0
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    animation: false,
                    interaction: { mode: 'index', intersect: false },
                    plugins: {
                        legend: { display: false },
                        tooltip: {
                            backgroundColor: initialColors.tooltipBg,
                            titleColor: initialColors.tooltipText,
                            bodyColor: initialColors.tooltipText,
                            borderColor: initialColors.tooltipBorder,
                            borderWidth: 1,
                            padding: 10,
                            boxPadding: 4,
                            usePointStyle: true,
                            titleFont: { family: 'Plus Jakarta Sans', weight: 'bold' },
                            bodyFont: { family: 'JetBrains Mono' }
                        }
                    },
                    scales: {
                        x: {
                            ticks: { color: initialColors.ticks, font: { family: 'JetBrains Mono', size: 10 }, maxTicksLimit: 6 },
                            grid: { color: initialColors.grid }
                        },
                        yPpm: {
                            type: 'linear',
                            position: 'left',
                            suggestedMin: 350,
                            suggestedMax: 1200,
                            ticks: {
                                color: initialColors.ticks,
                                font: { family: 'JetBrains Mono', size: 11 },
                                callback: v => v + ' ppm'
                            },
                            grid: { color: initialColors.grid }
                        },
                        yRaw: {
                            type: 'linear',
                            position: 'right',
                            min: 0,
                            suggestedMax: 1200,
                            ticks: {
                                color: '#94a3b8',
                                font: { family: 'JetBrains Mono', size: 10 }
                            },
                            title: { display: true, text: 'Raw', color: '#94a3b8', font: { family: 'JetBrains Mono', size: 10 } },
                            grid: { drawOnChartArea: false }
                        }
                    }
                }
            });

            // Synchronize Charts on Dark/Light Theme Switch
            function updateChartThemes() {
                const dark = isDarkMode();
                const colors = getChartThemeColors(dark);

                // Climate Chart
                climateChart.options.scales.x.ticks.color = colors.ticks;
                climateChart.options.scales.x.grid.color = colors.grid;
                climateChart.options.scales.yTemp.grid.color = colors.grid;
                climateChart.options.plugins.tooltip.backgroundColor = colors.tooltipBg;
                climateChart.options.plugins.tooltip.titleColor = colors.tooltipText;
                climateChart.options.plugins.tooltip.bodyColor = colors.tooltipText;
                climateChart.options.plugins.tooltip.borderColor = colors.tooltipBorder;
                climateChart.update('none');

                // Air Chart
                airChart.options.scales.x.ticks.color = colors.ticks;
                airChart.options.scales.x.grid.color = colors.grid;
                airChart.options.scales.yPpm.ticks.color = colors.ticks;
                airChart.options.scales.yPpm.grid.color = colors.grid;
                airChart.options.plugins.tooltip.backgroundColor = colors.tooltipBg;
                airChart.options.plugins.tooltip.titleColor = colors.tooltipText;
                airChart.options.plugins.tooltip.bodyColor = colors.tooltipText;
                airChart.options.plugins.tooltip.borderColor = colors.tooltipBorder;
                airChart.update('none');
            }

            // Apply detected theme
            applyTheme(initDark);

            // Floating Website Notification Helper
            let webToastTimer = null;
            const webToast = document.getElementById('web-alert-toast');
            const webToastTitle = document.getElementById('web-alert-title');
            const webToastMsg = document.getElementById('web-alert-msg');
            const webToastMeta = document.getElementById('web-alert-meta');
            const webToastClose = document.getElementById('web-alert-close');
            if (webToastClose && webToast) {
                webToastClose.addEventListener('click', () => webToast.classList.add('hidden'));
            }

            function showWebNotification(title, message) {
                if (!webToast) return;
                webToastTitle.textContent = title;
                webToastMsg.textContent = message;
                webToastMeta.textContent = `${clock()} • ${hardwareConnected ? 'Telegram alert triggered (@SemibotSys_Bot)' : 'Local alert (connect ESP32 for Telegram)'}`;
                webToast.classList.remove('hidden');
                if (webToastTimer) clearTimeout(webToastTimer);
                webToastTimer = setTimeout(() => { webToast.classList.add('hidden'); }, 8000);
            }

            // Event Logger & Incident Stream
            function addEvent(message, type = 'warning') {
                if (empty) empty.hidden = true;
                values.activeIncidents++;
                alertCounterEl.textContent = values.activeIncidents;

                playAlertChime();

                const isWarning = type === 'warning';
                if (isWarning) {
                    showWebNotification('SEMS Threshold Alert', message);
                }
                const item = document.createElement('li');
                item.className = `rounded-xl border p-3.5 transition flex items-start gap-3 ${
                    isWarning
                        ? 'border-amber-300 bg-amber-50/80 dark:border-amber-800/80 dark:bg-amber-950/40'
                        : 'border-emerald-300 bg-emerald-50/80 dark:border-emerald-800/80 dark:bg-emerald-950/40'
                }`;
                item.dataset.type = type;

                const iconBox = document.createElement('div');
                iconBox.className = `p-1.5 rounded-lg shrink-0 ${
                    isWarning ? 'bg-amber-500 text-white' : 'bg-emerald-500 text-white'
                }`;
                iconBox.innerHTML = isWarning
                    ? '<svg xmlns="http://www.w3.org/2000/svg" class="w-3.5 h-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>'
                    : '<svg xmlns="http://www.w3.org/2000/svg" class="w-3.5 h-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>';

                const body = document.createElement('div');
                body.className = 'flex-1 min-w-0';

                const title = document.createElement('strong');
                title.className = `block text-xs font-bold leading-tight ${
                    isWarning ? 'text-amber-900 dark:text-amber-200' : 'text-emerald-900 dark:text-emerald-200'
                }`;
                title.textContent = message;

                const meta = document.createElement('span');
                meta.className = 'block mt-1 text-[11px] text-slate-500 dark:text-slate-400 mono flex items-center gap-2';
                meta.innerHTML = `<span>${clock()}</span> &bull; <span>Dispatched to Telegram Channel</span>`;

                body.append(title, meta);
                item.append(iconBox, body);
                list.prepend(item);

                // Prune list
                const items = list.querySelectorAll('li:not([hidden])');
                if (items.length > 20) items[items.length - 1].remove();

                // Update Telegram Preview Box
                const tgTitle = document.getElementById('tg-preview-title');
                const tgCond = document.getElementById('tg-preview-condition');
                const tgVal = document.getElementById('tg-preview-val');
                const tgLimit = document.getElementById('tg-preview-limit');
                const tgTime = document.getElementById('tg-preview-time');

                if (tgTitle) tgTitle.textContent = isWarning ? 'LIMIT VIOLATION DETECTED' : 'SYSTEM TEST PING';
                if (tgCond) tgCond.textContent = message;
                if (tgTime) tgTime.textContent = clock();
                if (message.includes('Temperature')) {
                    if (tgVal) tgVal.textContent = values.temperature.toFixed(1) + ' °C';
                    if (tgLimit) tgLimit.textContent = limits.temperature.toFixed(1) + ' °C';
                } else if (message.includes('MQ-135') || message.includes('Gas')) {
                    if (tgVal) tgVal.textContent = Math.round(values.airPpm || mq135PpmFromRaw(values.air)) + ' ppm (Raw ' + values.air + ')';
                    if (tgLimit) tgLimit.textContent = limits.air + ' ppm';
                }
            }

            // Hardware & Simulation State
            let hardwareConnected = false;
            let espHost = localStorage.getItem('sems_esp_host') || '';
            const espStatusBtn = document.getElementById('esp-status-btn');
            const espStatusDot = document.getElementById('esp-status-dot');
            const espStatusText = document.getElementById('esp-status-text');

            function updateHardwareBadge(connected, ip = '') {
                hardwareConnected = connected;
                if (espStatusDot && espStatusText) {
                    if (connected) {
                        espStatusDot.className = 'w-2.5 h-2.5 rounded-full bg-emerald-500 ring-4 ring-emerald-500/20';
                        espStatusText.textContent = ip ? `ESP32: ${ip}` : 'ESP32: Live Hardware';
                        espStatusText.className = 'text-emerald-700 dark:text-emerald-400 font-bold';
                    } else {
                        espStatusDot.className = 'w-2.5 h-2.5 rounded-full bg-amber-500 ring-2 ring-amber-500/20';
                        espStatusText.textContent = espHost ? `ESP32: Offline (${espHost.replace(/^https?:\/\//, '')})` : 'ESP32: Demo Mode';
                        espStatusText.className = 'text-slate-600 dark:text-slate-300 font-semibold';
                    }
                }
            }

            if (espStatusBtn) {
                espStatusBtn.addEventListener('click', () => {
                    const current = espHost || (window.location.protocol.startsWith('http') ? window.location.origin : 'http://192.168.1.105');
                    const target = prompt('Enter ESP32 IP address or Hostname (e.g. http://192.168.1.105 or http://sems.local):\nLeave blank to auto-detect from current web origin:', current);
                    if (target !== null) {
                        espHost = target.trim().replace(/\/+$/, '');
                        localStorage.setItem('sems_esp_host', espHost);
                        feedback.textContent = espHost ? `Target ESP32 set to: ${espHost}` : 'ESP32 target reset to auto-detect.';
                        sample();
                    }
                });
            }

            // Demo Simulation Mode State & UI Helper
            let demoSimActive = false;
            let consecutiveFetchFails = 0;
            const demoSimBtn = document.getElementById('demo-sim-btn');
            const demoSimBtnText = document.getElementById('demo-sim-btn-text');

            function setDemoSimUI(active) {
                demoSimActive = active;
                if (!demoSimBtn || !demoSimBtnText) return;
                if (active) {
                    demoSimBtn.className = 'shrink-0 inline-flex items-center justify-center gap-2 py-2.5 px-4 rounded-xl font-bold text-xs bg-rose-600 hover:bg-rose-500 text-white shadow-md shadow-rose-600/25 ring-2 ring-rose-400/50 transition';
                    demoSimBtnText.textContent = 'Stop Simulation (Restore Sensors)';
                } else {
                    demoSimBtn.className = 'shrink-0 inline-flex items-center justify-center gap-2 py-2.5 px-4 rounded-xl font-bold text-xs bg-violet-600 hover:bg-violet-500 text-white shadow-md shadow-violet-600/20 transition';
                    demoSimBtnText.textContent = 'Trigger Demo Simulation';
                }
            }

            // Real-Time Telemetry Pipeline (Hardware Fetch with Simulation Fallback)
            async function sample() {
                let fetched = false;

                // Attempt to fetch live hardware metrics from ESP32 REST API
                try {
                    const controller = new AbortController();
                    const tid = setTimeout(() => controller.abort(), 900);
                    const targetUrl = (espHost ? espHost : '') + '/api/data';
                    const res = await fetch(targetUrl, { signal: controller.signal });
                    clearTimeout(tid);

                    if (res.ok) {
                        const data = await res.json();
                        consecutiveFetchFails = 0;
                        values.temperature = Number(data.temperature);
                        values.humidity = Number(data.humidity);
                        values.air = Number(data.air_adc);
                        values.airPpm = (data.air_ppm !== undefined) ? Number(data.air_ppm) : mq135PpmFromRaw(values.air);

                        if (data.packet_count !== undefined) values.packets = Number(data.packet_count);
                        if (data.alert_count !== undefined) values.activeIncidents = Number(data.alert_count);
                        if (data.uptime_sec !== undefined) espUptimeSec = Number(data.uptime_sec);
                        if (data.simulation !== undefined) setDemoSimUI(Boolean(data.simulation));

                        if (data.temp_limit && !tempInput.matches(':focus')) {
                            limits.temperature = Number(data.temp_limit);
                            tempInput.value = limits.temperature;
                            tempSlider.value = limits.temperature;
                            tempDisplay.textContent = limits.temperature.toFixed(1);
                            tempStatLimit.textContent = limits.temperature.toFixed(1) + '°C';
                        }
                        if (data.air_limit && !airInput.matches(':focus')) {
                            limits.air = Number(data.air_limit);
                            airInput.value = limits.air;
                            airSlider.value = limits.air;
                            airDisplay.textContent = limits.air;
                            airStatLimit.textContent = limits.air + ' ppm';
                            history.airThreshold = history.airThreshold.map(() => limits.air);
                        }

                        if (data.wifi_rssi && document.getElementById('wifi-rssi-val')) {
                            document.getElementById('wifi-rssi-val').textContent = `${data.wifi_rssi} dBm (ESP32 Live)`;
                        }
                        if (data.ip && document.getElementById('tg-preview-ip')) {
                            document.getElementById('tg-preview-ip').textContent = data.ip;
                        }
                        const tgLinkEl = document.getElementById('tg-link-status');
                        if (tgLinkEl && data.chat_id_set !== undefined) {
                            if (data.chat_id_set) {
                                tgLinkEl.className = 'text-xs font-semibold text-emerald-400';
                                tgLinkEl.textContent = '✅ Telegram Chat Linked';
                            } else {
                                tgLinkEl.className = 'text-xs font-semibold text-amber-400';
                                tgLinkEl.innerHTML = '⚠️ Send <strong>/start</strong> to @SemibotSys_Bot';
                            }
                        }

                        updateHardwareBadge(true, data.ip);
                        fetched = true;
                    }
                } catch (e) {
                    consecutiveFetchFails++;
                }

                if (!fetched) {
                    if (!hardwareConnected || consecutiveFetchFails >= 3) {
                        updateHardwareBadge(false);
                        if (demoSimActive) {
                            // Demo Simulation spike (offline preview mode)
                            values.temperature = Math.round((limits.temperature + 6.5 + (Math.random() - 0.5) * 0.6) * 10) / 10;
                            values.humidity = Math.round(clamp(76 + (Math.random() - 0.5) * 3, 65, 88));
                            values.air = 2150;
                            values.airPpm = Math.min(4800, Math.round(limits.air + 450 + (Math.random() - 0.5) * 80));
                            values.packets++;
                        } else {
                            // Realistic walking telemetry simulation step
                            values.temperature = Math.round(clamp(values.temperature + (Math.random() - 0.48) * 0.45, 21.0, 36.0) * 10) / 10;
                            values.humidity = Math.round(clamp(values.humidity + (Math.random() - 0.5) * 2.2, 32, 78));
                            values.air = Math.round(clamp(values.air + (Math.random() - 0.49) * 35, 260, 920));
                            values.airPpm = mq135PpmFromRaw(values.air);
                            values.packets++;
                        }
                    } else {
                        return; // Brief single-packet hiccup: keep real sensor reading instead of injecting fake data
                    }
                }

                // Statistics
                stats.tempSum += values.temperature;
                stats.tempCount++;
                stats.tempMin = Math.min(stats.tempMin, values.temperature);
                stats.tempMax = Math.max(stats.tempMax, values.temperature);

                // Update Numerical DOM
                document.getElementById('temp-value').textContent = values.temperature.toFixed(1);
                document.getElementById('temp-f-value').textContent = cToF(values.temperature) + ' °F';
                document.getElementById('humidity-value').textContent = Math.round(values.humidity);
                document.getElementById('packet-counter').textContent = values.packets;

                // Dew point calculation
                const dp = calculateDewPoint(values.temperature, values.humidity);
                document.getElementById('dew-point-value').textContent = dp + '°C';

                // Average temperature
                const avg = (stats.tempSum / stats.tempCount).toFixed(1);
                document.getElementById('temp-stat-avg').textContent = avg + '°C';

                // Air quality: ppm as main value, raw sensor value as side print
                const displayPpm = Math.round(values.airPpm !== undefined ? values.airPpm : mq135PpmFromRaw(values.air));
                document.getElementById('air-value').textContent = displayPpm;
                document.getElementById('air-raw-value').textContent = values.air;

                // Update Telegram Preview Card dynamically
                const tgValEl = document.getElementById('tg-preview-val');
                const tgLimitEl = document.getElementById('tg-preview-limit');
                const tgCondEl = document.getElementById('tg-preview-condition');
                if (tgValEl && tgLimitEl && tgCondEl) {
                    if (values.temperature > limits.temperature) {
                        tgCondEl.textContent = 'High Temperature Alert (DHT22)';
                        tgValEl.textContent = `${values.temperature.toFixed(1)} °C`;
                        tgLimitEl.textContent = `${limits.temperature.toFixed(1)} °C`;
                    } else {
                        tgCondEl.textContent = displayPpm > limits.air ? 'MQ-135 Gas Reading above limit' : 'Live Sensor Telemetry Nominal';
                        tgValEl.textContent = `${displayPpm} ppm (Raw ${values.air})`;
                        tgLimitEl.textContent = `${limits.air} ppm`;
                    }
                }

                // Temperature Card Background & Status Badge (Green = Normal, Yellow = Warm, Red = Hot)
                const tempCard = document.getElementById('temp-card');
                const tempBadge = document.getElementById('temp-status-badge');
                if (values.temperature > limits.temperature) {
                    tempCard.className = AIR_CARD_BASE + 'bg-red-100 border-red-500 dark:bg-red-950/70 dark:border-red-600';
                    tempBadge.className = 'px-2 py-0.5 rounded-full text-[11px] font-bold bg-red-600 text-white';
                    tempBadge.textContent = 'Hot';
                } else if (values.temperature >= limits.temperature - 3) {
                    tempCard.className = AIR_CARD_BASE + 'bg-yellow-100 border-yellow-400 dark:bg-yellow-900/40 dark:border-yellow-500';
                    tempBadge.className = 'px-2 py-0.5 rounded-full text-[11px] font-bold bg-yellow-400 text-yellow-950';
                    tempBadge.textContent = 'Warm';
                } else {
                    tempCard.className = AIR_CARD_BASE + 'bg-emerald-100 border-emerald-400 dark:bg-emerald-900/50 dark:border-emerald-600';
                    tempBadge.className = 'px-2 py-0.5 rounded-full text-[11px] font-bold bg-emerald-600 text-white';
                    tempBadge.textContent = 'Normal';
                }

                // Air Quality Rating: colour-fill the whole Air Quality card
                const airLevel = airQualityLevel(displayPpm);
                const airStyle = AIR_QUALITY_STYLES[airLevel];
                const airCard = document.getElementById('air-card');
                const airBadge = document.getElementById('air-status-badge');
                const airRating = document.getElementById('air-rating-text');
                airCard.className = AIR_CARD_BASE + airStyle.card;
                airBadge.className = 'px-2 py-0.5 rounded-full text-[11px] font-bold ' + airStyle.badge;
                airBadge.textContent = airLevel;
                airRating.className = 'font-bold ' + airStyle.text;
                airRating.textContent = airStyle.label;
                airStatLimit.className = displayPpm > limits.air
                    ? 'text-red-700 dark:text-red-300 mono font-bold'
                    : 'text-slate-800 dark:text-slate-100 mono';

                // Chart ppm line follows the rating colour
                airChart.data.datasets[0].borderColor = airStyle.line;
                airChart.data.datasets[0].pointBackgroundColor = airStyle.line;
                airChart.data.datasets[0].backgroundColor = airStyle.fill;

                // Append to Chart History
                const timeLabel = clock();
                history.labels.push(timeLabel);
                history.temperature.push(values.temperature);
                history.humidity.push(values.humidity);
                history.air.push(values.air);
                history.airPpm.push(displayPpm);
                history.airThreshold.push(limits.air);

                // Keep rolling buffer of 14 readings
                if (history.labels.length > 14) {
                    history.labels.shift();
                    history.temperature.shift();
                    history.humidity.shift();
                    history.air.shift();
                    history.airPpm.shift();
                    history.airThreshold.shift();
                }

                climateChart.update('none');
                airChart.update('none');

                                // Incident Detection & Autonomous Alert Triggering
                const now = Date.now();
                if (values.temperature > limits.temperature && (now - lastAlert.temperature > 15000)) {
                    addEvent(`High Temperature Alert: ${values.temperature.toFixed(1)} °C exceeds limit (${limits.temperature.toFixed(1)} °C)`, 'warning');
                    lastAlert.temperature = now;
                }

                if (displayPpm > limits.air && (now - lastAlert.air > 15000)) {
                    addEvent(`Air Quality Alert: Gas level ${displayPpm} ppm exceeds limit (${limits.air} ppm) • Raw: ${values.air} (${airLevel})`, 'warning');
                    lastAlert.air = now;
                }
            }

            // Initial telemetry read & fast live loop (500ms interval, non-overlapping)
            let sampleInFlight = false;
            async function pollLive() {
                if (!running || sampleInFlight) return;
                sampleInFlight = true;
                try { await sample(); } finally { sampleInFlight = false; }
            }
            pollLive();
            setInterval(pollLive, 500);

            // Clock & Uptime Timer (uses ESP32 uptime when connected, browser elapsed when offline)
            let espUptimeSec = null;
            function fmtUptime(sec) {
                const hrs = String(Math.floor(sec / 3600)).padStart(2, '0');
                const mins = String(Math.floor((sec % 3600) / 60)).padStart(2, '0');
                const secs = String(sec % 60).padStart(2, '0');
                return `${hrs}:${mins}:${secs}`;
            }
            if (liveClockEl) liveClockEl.textContent = clock();
            setInterval(() => {
                if (liveClockEl) liveClockEl.textContent = clock();
                if (uptimeEl) {
                    const sec = (hardwareConnected && espUptimeSec !== null)
                        ? espUptimeSec
                        : Math.floor((Date.now() - startTime) / 1000);
                    uptimeEl.textContent = fmtUptime(sec);
                }
            }, 1000);

            // Theme Switcher Click Handler
            themeToggleBtn.addEventListener('click', () => {
                const willBeDark = !isDarkMode();
                applyTheme(willBeDark);

                feedback.textContent = willBeDark ? 'Dark theme enabled.' : 'Light theme enabled.';
                setTimeout(() => { if (feedback.textContent.includes('theme')) feedback.textContent = ''; }, 3000);
            });

            // Simulation Pause / Resume Handler
            document.getElementById('toggle-simulation').addEventListener('click', () => {
                running = !running;
                document.getElementById('pause-text').classList.toggle('hidden', !running);
                document.getElementById('resume-text').classList.toggle('hidden', running);
                document.getElementById('status-running').classList.toggle('hidden', !running);
                document.getElementById('status-paused').classList.toggle('hidden', running);

                const dot = document.getElementById('status-dot');
                const simIcon = document.getElementById('sim-icon');
                if (running) {
                    dot.className = 'w-2.5 h-2.5 rounded-full bg-emerald-500 ring-4 ring-emerald-500/20';
                    if (simIcon) simIcon.setAttribute('data-lucide', 'pause');
                    feedback.textContent = 'Telemetry pipeline running at 1.0 Hz.';
                } else {
                    dot.className = 'w-2.5 h-2.5 rounded-full bg-amber-500 ring-4 ring-amber-500/20';
                    if (simIcon) simIcon.setAttribute('data-lucide', 'play');
                    feedback.textContent = 'Telemetry pipeline paused.';
                }
                lucide.createIcons();
            });

            // Audio Alert Mute / Unmute
            document.getElementById('audio-toggle').addEventListener('click', () => {
                audioMuted = !audioMuted;
                const icon = document.getElementById('audio-icon');
                if (audioMuted) {
                    icon.setAttribute('data-lucide', 'volume-x');
                    icon.className = 'w-4 h-4 text-slate-400';
                    feedback.textContent = 'Alert chime muted.';
                } else {
                    icon.setAttribute('data-lucide', 'volume-2');
                    icon.className = 'w-4 h-4 text-emerald-600 dark:text-emerald-400';
                    playAlertChime();
                    feedback.textContent = 'Alert chime unmuted.';
                }
                lucide.createIcons();
                setTimeout(() => { if (feedback.textContent.includes('chime')) feedback.textContent = ''; }, 2500);
            });

            // Threshold Form Synchronization (Inputs & Sliders)
            const tempInput = document.getElementById('temp-threshold');
            const tempSlider = document.getElementById('temp-threshold-slider');
            const tempDisplay = document.getElementById('temp-threshold-display');
            const tempStatLimit = document.getElementById('temp-stat-limit');

            const airInput = document.getElementById('air-threshold');
            const airSlider = document.getElementById('air-threshold-slider');
            const airDisplay = document.getElementById('air-threshold-display');
            const airStatLimit = document.getElementById('air-stat-limit');

            tempSlider.addEventListener('input', e => {
                tempInput.value = e.target.value;
                tempDisplay.textContent = Number(e.target.value).toFixed(1);
            });
            tempInput.addEventListener('input', e => {
                tempSlider.value = e.target.value;
                tempDisplay.textContent = Number(e.target.value).toFixed(1);
            });

            airSlider.addEventListener('input', e => {
                airInput.value = e.target.value;
                airDisplay.textContent = e.target.value;
            });
            airInput.addEventListener('input', e => {
                airSlider.value = e.target.value;
                airDisplay.textContent = e.target.value;
            });

            // Preset Buttons Handler
            const customEnvBox = document.getElementById('custom-env-box');
            const customEnvInput = document.getElementById('custom-env-input');
            const PRESET_PROFILES = {
                construction: { name: 'Construction Site', temp: 35.0, air: 1500 },
                factory:      { name: 'Factory',           temp: 33.0, air: 1200 },
                warehouse:    { name: 'Warehouse',         temp: 30.0, air: 900 },
                lab:          { name: 'Laboratory',        temp: 25.0, air: 600 }
            };

            if (customEnvInput) {
                const savedEnv = localStorage.getItem('sems_custom_env');
                if (savedEnv) customEnvInput.value = savedEnv;
                customEnvInput.addEventListener('input', () => {
                    localStorage.setItem('sems_custom_env', customEnvInput.value.trim());
                });
            }

            document.querySelectorAll('.preset-btn').forEach(btn => {
                btn.addEventListener('click', () => {
                    const preset = btn.dataset.preset;
                    document.querySelectorAll('.preset-btn').forEach(b => b.classList.remove('ring-2', 'ring-emerald-500'));
                    btn.classList.add('ring-2', 'ring-emerald-500');

                    if (preset === 'others') {
                        if (customEnvBox) customEnvBox.classList.remove('hidden');
                        if (customEnvInput) customEnvInput.focus();
                        const loc = (customEnvInput && customEnvInput.value.trim()) ? customEnvInput.value.trim() : 'Custom Location';
                        feedback.textContent = `Custom mode (${loc}): adjust Temp & PPM limits and click Apply.`;
                        return;
                    }

                    if (customEnvBox) customEnvBox.classList.add('hidden');
                    const cfg = PRESET_PROFILES[preset];
                    if (cfg) {
                        tempInput.value = cfg.temp;
                        airInput.value = cfg.air;
                        tempSlider.value = tempInput.value;
                        airSlider.value = airInput.value;
                        tempDisplay.textContent = Number(tempInput.value).toFixed(1);
                        airDisplay.textContent = airInput.value;
                        feedback.textContent = `Applied ${cfg.name} profile (${cfg.temp}°C, ${cfg.air} ppm). Click Apply to save.`;
                    }
                });
            });

            // Form Submit / Apply Thresholds
            document.getElementById('threshold-form').addEventListener('submit', async e => {
                e.preventDefault();
                limits.temperature = Number(tempInput.value);
                limits.air = Number(airInput.value);

                tempStatLimit.textContent = limits.temperature.toFixed(1) + '°C';
                airStatLimit.textContent = limits.air + ' ppm';

                // Update chart threshold line
                history.airThreshold = history.airThreshold.map(() => limits.air);
                airChart.update('none');

                // Reset alert cooldowns so if current readings exceed the new threshold, notify immediately
                lastAlert.temperature = 0;
                lastAlert.air = 0;

                // Synchronize with ESP32 if connected (triggers immediate Telegram check on ESP32)
                if (hardwareConnected) {
                    try {
                        const targetUrl = (espHost ? espHost : '') + `/api/thresholds?temp=${limits.temperature}&air=${limits.air}`;
                        const res = await fetch(targetUrl, { method: 'POST' });
                        if (res.ok) {
                            const info = await res.json();
                            if (info.temp_alert || info.air_alert) {
                                if (info.telegram_sent) {
                                    feedback.textContent = 'Thresholds saved — 🚨 Threshold exceeded! Telegram alert sent (@SemibotSys_Bot).';
                                } else if (!info.chat_linked) {
                                    feedback.textContent = 'Thresholds saved — ⚠️ Exceeded! Open @SemibotSys_Bot in Telegram and send /start to link your chat.';
                                } else {
                                    feedback.textContent = 'Thresholds saved — ⚠️ Exceeded! Telegram send failed (check ESP32 Wi-Fi internet access).';
                                }
                            } else {
                                feedback.textContent = 'Thresholds updated & synchronized with ESP32.';
                            }
                        } else {
                            feedback.textContent = 'Thresholds updated locally (ESP32 sync failed).';
                        }
                    } catch (err) {
                        feedback.textContent = 'Thresholds updated locally (ESP32 unreachable).';
                    }
                } else {
                    feedback.textContent = 'Thresholds updated successfully.';
                }

                // Re-evaluate current reading against new limits right away (updates cards + web notification)
                await sample();
                setTimeout(() => { if (feedback.textContent.includes('Thresholds')) feedback.textContent = ''; }, 4500);
            });

            // Simulate Manual Test Alert
            document.getElementById('test-alert').addEventListener('click', () => {
                addEvent('Test Incident Trigger: Operator simulated Telegram webhook dispatch.', 'warning');
                
                if (hardwareConnected) {
                    const targetUrl = (espHost ? espHost : '') + '/api/test_alert';
                    fetch(targetUrl, { method: 'POST' })
                        .then(r => r.json())
                        .then(res => {
                            if (res.status === 'sent') {
                                feedback.textContent = '✅ Real alert dispatched to Telegram (@SemibotSys_Bot)!';
                            } else {
                                feedback.textContent = '⚠️ Logged. Send /start to @SemibotSys_Bot on Telegram to link your chat!';
                            }
                        })
                        .catch(() => {
                            feedback.textContent = 'Alert logged locally. ESP32 Telegram webhook unreachable.';
                        });
                } else {
                    feedback.textContent = 'Test incident logged. Connect ESP32 to push real messages to Telegram.';
                }
                setTimeout(() => { if (feedback.textContent.includes('Test') || feedback.textContent.includes('alert')) feedback.textContent = ''; }, 4500);
            });

            // Quick Spike Temperature button
            document.getElementById('spike-temp-btn').addEventListener('click', () => {
                values.temperature = limits.temperature + 2.5;
                sample();
                feedback.textContent = 'Temperature spiked for incident test.';
            });

            // Trigger Demo Simulation Button (Connected to ESP32 /api/simulate & Telegram Bot)
            if (demoSimBtn) {
                demoSimBtn.addEventListener('click', async () => {
                    const nextState = !demoSimActive;
                    setDemoSimUI(nextState);

                    if (nextState) {
                        addEvent('🧪 Demo Simulation Triggered: Injecting environmental spike for demonstration.', 'warning');
                        feedback.textContent = 'Demo Simulation ACTIVE — triggering graphs, OLED & Telegram alert...';
                    } else {
                        addEvent('✅ Demo Simulation Stopped: Restoring live DHT22 & MQ-135 sensor telemetry.', 'info');
                        feedback.textContent = 'Demo Simulation stopped. Restoring live sensor data...';
                    }

                    if (hardwareConnected) {
                        try {
                            const targetUrl = (espHost ? espHost : '') + `/api/simulate?active=${nextState ? 1 : 0}`;
                            const res = await fetch(targetUrl, { method: 'POST' });
                            if (res.ok) {
                                const data = await res.json();
                                if (nextState && data.telegram_sent) {
                                    feedback.textContent = '🧪 Demo Simulation ACTIVE on ESP32 & Telegram alert sent (@SemibotSys_Bot)!';
                                } else if (nextState) {
                                    feedback.textContent = '🧪 Demo Simulation ACTIVE on ESP32! (Send /start to @SemibotSys_Bot to link Telegram)';
                                } else {
                                    feedback.textContent = '✅ Restored live DHT22 & MQ-135 sensor readings on ESP32.';
                                }
                            }
                        } catch (err) {
                            feedback.textContent = nextState ? 'Demo Simulation active locally (ESP32 unreachable).' : 'Demo Simulation stopped.';
                        }
                    }

                    await sample();
                    setTimeout(() => {
                        if (feedback.textContent.includes('Demo') || feedback.textContent.includes('Restored')) {
                            feedback.textContent = '';
                        }
                    }, 5000);
                });
            }

            // Clear Log button
            document.getElementById('clear-activity-btn').addEventListener('click', () => {
                list.innerHTML = '';
                if (empty) {
                    empty.hidden = false;
                    list.appendChild(empty);
                }
                values.activeIncidents = 0;
                alertCounterEl.textContent = '0';
                feedback.textContent = 'Audit log cleared.';
                setTimeout(() => { feedback.textContent = ''; }, 2000);
            });

            // Log Filter Tabs
            const filterAllBtn = document.getElementById('log-filter-all');
            const filterAlertsBtn = document.getElementById('log-filter-alerts');

            filterAllBtn.addEventListener('click', () => {
                alertFilter = 'all';
                filterAllBtn.className = 'px-3 py-1 rounded-full bg-slate-900 text-white dark:bg-slate-100 dark:text-slate-900 font-semibold';
                filterAlertsBtn.className = 'px-3 py-1 rounded-full bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-300 hover:bg-slate-200 font-medium';
                list.querySelectorAll('li[data-type]').forEach(el => el.classList.remove('hidden'));
            });

            filterAlertsBtn.addEventListener('click', () => {
                alertFilter = 'alerts';
                filterAlertsBtn.className = 'px-3 py-1 rounded-full bg-slate-900 text-white dark:bg-slate-100 dark:text-slate-900 font-semibold';
                filterAllBtn.className = 'px-3 py-1 rounded-full bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-300 hover:bg-slate-200 font-medium';
                list.querySelectorAll('li[data-type]').forEach(el => {
                    if (el.dataset.type !== 'warning') el.classList.add('hidden');
                    else el.classList.remove('hidden');
                });
            });

            // CSV Export Handler
            document.getElementById('export-csv-btn').addEventListener('click', () => {
                let csv = 'Timestamp,Temperature_C,Humidity_RH,Air_PPM,Air_MQ135_Raw,Temp_Limit_C,Air_Limit_PPM\n';
                for (let i = 0; i < history.labels.length; i++) {
                    csv += `"${history.labels[i]}",${history.temperature[i]},${history.humidity[i]},${history.airPpm[i]},${history.air[i]},${limits.temperature},${limits.air}\n`;
                }
                const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = `sems_telemetry_${Date.now()}.csv`;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
                feedback.textContent = 'Telemetry exported to CSV.';
                setTimeout(() => { feedback.textContent = ''; }, 3000);
            });

            // Initialize Lucide icons on page load
            if (window.lucide) {
                lucide.createIcons();
            }
        })();
    </script>
</body>

</html>
)SEMS_HTML";

// ======================================================================================
// 4. SENSOR CALCULATION FUNCTIONS
// ======================================================================================
// ---- MQ-135 measurement (same method as Esp32GasTest.ino) ----

// Averaged raw ADC (0-4095) and calibrated AOUT voltage in a single sampling pass
void readMq135Averaged(int &rawOut, float &voltOut) {
  long sumRaw = 0;
  uint32_t sumMv = 0;
  for (uint16_t i = 0; i < MQ_SAMPLES; i++) {
    sumRaw += analogRead(MQ135_PIN);
    sumMv  += analogReadMilliVolts(MQ135_PIN);   // ESP32 eFuse-calibrated millivolts
    delay(MQ_SAMPLE_DELAY);
  }
  rawOut  = (int)(sumRaw / MQ_SAMPLES);
  float vAdc = (sumMv / (float)MQ_SAMPLES) / 1000.0f;
  voltOut = vAdc / DIVIDER_RATIO;                // Voltage at the sensor AOUT
}

// Sensor resistance Rs (kOhm)
float calculateRs(float vOut) {
  if (vOut <= 0.01f) return -1.0f;               // No signal / not connected
  float vc = ADC_VREF / DIVIDER_RATIO;
  float rs = RL_VALUE * (vc - vOut) / vOut;
  return (rs > 0) ? rs : -1.0f;
}

// Gas concentration (ppm, CO2-equivalent)
float calculatePPM(float rs) {
  if (rs <= 0) return ATMOSPHERIC_CO2_BASELINE;
  float ratio = rs / R0_CLEAN_AIR;
  if (ratio <= 0) return ATMOSPHERIC_CO2_BASELINE;
  float ppm = (GAS_A * pow(ratio, GAS_B)) + ATMOSPHERIC_CO2_BASELINE;
  return constrain(ppm, 350.0f, 5000.0f);
}

// Air quality rating (matches the web dashboard colours)
const char* mq135QualityLabel(float ppm) {
  if (ppm < 600)  return "GOOD";
  if (ppm < 1000) return "MODERATE";
  if (ppm < 2000) return "POOR";
  return "HAZARDOUS";
}

void readMq135() {
  readMq135Averaged(g_mq135Raw, g_mq135Voltage);
  g_mq135Rs    = calculateRs(g_mq135Voltage);
  g_mq135Ratio = (g_mq135Rs > 0) ? g_mq135Rs / R0_CLEAN_AIR : 0.0f;
  g_ppm        = calculatePPM(g_mq135Rs);
}

// Wiring sanity check at boot
void checkMq135Wiring() {
  int raw; float v;
  readMq135Averaged(raw, v);
  Serial.printf("[MQ-135 CHECK] Initial raw = %d (%.3f V)\n", raw, v);
  if (raw <= 10)        Serial.println(F("[MQ-135 WARN] Raw ~0 -> AOUT not connected, wrong pin, or no power."));
  else if (raw >= 4085) Serial.println(F("[MQ-135 WARN] Raw saturated (4095) -> AOUT above 3.3V. Add a voltage divider!"));
  else                  Serial.println(F("[MQ-135 OK] Analog signal detected."));
}

// R0 calibration in clean air (send 'c' in Serial Monitor)
void calibrateR0() {
  Serial.println(F("\n[CALIBRATION] Keep the MQ-135 in fresh/clean air..."));
  float sumRs = 0; uint16_t valid = 0;
  for (uint16_t i = 0; i < MQ_CALIB_SAMPLES; i++) {
    int raw; float v;
    readMq135Averaged(raw, v);
    float rs = calculateRs(v);
    if (rs > 0) { sumRs += rs; valid++; }
    server.handleClient();   // keep dashboard responsive during calibration
    delay(100);
  }
  if (valid == 0) { Serial.println(F("[CALIBRATION] FAILED - no valid signal. Check wiring.")); return; }
  R0_CLEAN_AIR = sumRs / valid;
  Serial.printf("[CALIBRATION] Done. New R0 = %.2f kOhm (paste into R0_CLEAN_AIR to keep after reboot)\n\n", R0_CLEAN_AIR);
}

void updateSensors() {
  if (g_demoSimulation) {
    // Demo Simulation Mode: inject a raw value above the configured limit and run it through the real formula
    g_temp = g_tempLimit + 6.5f;
    g_hum  = 78.0f;
    g_mq135Raw     = 2150;
    g_mq135Voltage = ((g_mq135Raw / 4095.0f) * ADC_VREF) / DIVIDER_RATIO;
    g_mq135Rs      = calculateRs(g_mq135Voltage);
    g_mq135Ratio   = (g_mq135Rs > 0) ? g_mq135Rs / R0_CLEAN_AIR : 0.0f;
    g_ppm          = constrain((float)g_airLimit + 450.0f, 1200.0f, 4500.0f);
    g_dhtSuccess = true;
    g_packetCounter++;
    Serial.printf("[DEMO SIM] Temp: %.1f C | Hum: %.1f %% | Gas: %.0f ppm (%s) | Raw: %d\n",
                  g_temp, g_hum, g_ppm, mq135QualityLabel(g_ppm), g_mq135Raw);
    return;
  }

  // Real Physical Sensor Readings: DHT22 every 2s (hardware minimum), MQ-135 every call (500ms)
  uint32_t now = millis();
  if (g_lastDhtReadMs == 0 || (now - g_lastDhtReadMs >= 2000)) {
    g_lastDhtReadMs = now;
    float newHum = dht.readHumidity();
    float newTemp = dht.readTemperature();
    if (isnan(newHum) || isnan(newTemp)) {
      g_dhtSuccess = false;   // keep last valid temp/hum so JSON stays valid
    } else {
      g_temp = newTemp;
      g_hum = newHum;
      g_dhtSuccess = true;
    }
  }

  readMq135();
  g_packetCounter++;

  Serial.print(F("[SENSOR READ] "));
  if (g_dhtSuccess) Serial.printf("Temp: %.1f C | Hum: %.1f %% | ", g_temp, g_hum);
  else              Serial.print(F("DHT22 Read Error! | "));
  if (g_mq135Rs > 0) {
    Serial.printf("Gas: %.0f ppm (%s) | Raw: %d | V: %.3f | Rs: %.2f kOhm | Rs/R0: %.3f\n",
                  g_ppm, mq135QualityLabel(g_ppm), g_mq135Raw, g_mq135Voltage, g_mq135Rs, g_mq135Ratio);
  } else {
    Serial.printf("MQ-135 NO SIGNAL (raw %d, %.3f V) - check wiring/power\n", g_mq135Raw, g_mq135Voltage);
  }
}

// ======================================================================================
// 5. TELEGRAM BOT PROTOCOL IMPLEMENTATION
// ======================================================================================
// Save Chat ID persistently in NVS flash so it survives ESP32 reboots
void saveChatId(const String& id) {
  if (id.length() == 0 || id == g_chatId) return;
  g_chatId = id;
  prefs.begin("sems", false);
  prefs.putString("chat_id", g_chatId);
  prefs.end();
  Serial.printf("[TELEGRAM] Saved Chat ID to NVS: %s\n", g_chatId.c_str());
}

// Send Telegram message with automatic plain-text fallback if Markdown parsing fails
bool sendTelegramSafe(const String& chatId, const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[TELEGRAM ERROR] Wi-Fi not connected."));
    return false;
  }
  if (chatId.length() == 0) {
    Serial.println(F("[TELEGRAM WARN] No Chat ID linked yet. Send /start to @SemibotSys_Bot."));
    return false;
  }
  if (bot.sendMessage(chatId, msg, "Markdown")) {
    Serial.println(F("[TELEGRAM] Alert sent successfully (Markdown)."));
    return true;
  }
  // Fallback: strip Markdown characters and send as plain text
  String plain = msg;
  plain.replace("*", "");
  plain.replace("_", "");
  plain.replace("`", "");
  if (bot.sendMessage(chatId, plain, "")) {
    Serial.println(F("[TELEGRAM] Alert sent successfully (plain-text fallback)."));
    return true;
  }
  Serial.println(F("[TELEGRAM ERROR] Failed to send message to Telegram API."));
  return false;
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.printf("[TELEGRAM INCOMING] From: %s (%s) | Msg: %s\n", from_name.c_str(), chat_id.c_str(), text.c_str());

    // Auto-save the chat ID persistently so notifications survive reboots!
    saveChatId(chat_id);

    if (text == "/start") {
      String welcome = "🌿 *SEMS - Smart Environmental Monitoring System*\n";
      welcome += "Hello, *" + from_name + "*! Node: *ESP32-NODE-01* is online.\n\n";
      welcome += "✅ *Linked to this chat!* Automatic environmental alerts will be delivered here.\n\n";
      welcome += "*Available Commands:*\n";
      welcome += "• `/status` - Live environmental metrics\n";
      welcome += "• `/temp` - Current DHT22 temperature\n";
      welcome += "• `/hum` - Current DHT22 humidity\n";
      welcome += "• `/air` - MQ-135 gas ppm & air quality\n";
      welcome += "• `/limits` - View configured alert limits\n";
      welcome += "• `/simulate` - Trigger Demo Simulation spike\n";
      welcome += "• `/stop` - Stop simulation & restore live sensors\n";
      welcome += "• `/test` - Trigger a test notification";
      bot.sendMessage(chat_id, welcome, "Markdown");
    }
    else if (text == "/status") {
      String stat = "📊 *SEMS LIVE TELEMETRY REPORT*\n";
      if (g_demoSimulation) stat += "🧪 *MODE: DEMO SIMULATION ACTIVE*\n";
      stat += "🌡️ Temperature: *" + String(g_temp, 1) + " °C* (Limit: " + String(g_tempLimit, 1) + " °C)\n";
      stat += "💧 Humidity: *" + String(g_hum, 1) + " %*\n";
      stat += "🌬️ Air Quality: *" + String(g_ppm, 0) + " ppm* (" + String(mq135QualityLabel(g_ppm)) + ") | Raw: " + String(g_mq135Raw) + "\n";
      stat += "📶 Wi-Fi RSSI: *" + String(WiFi.RSSI()) + " dBm*\n";
      stat += "⏱️ Uptime: *" + String((millis() - g_bootTimeMs) / 1000) + " sec*\n";
      stat += "\nSystem Status: " + String((g_temp > g_tempLimit || g_ppm > g_airLimit) ? "⚠️ *ALERT ACTIVE*" : "✅ *Nominal*");
      bot.sendMessage(chat_id, stat, "Markdown");
    }
    else if (text == "/temp") {
      bot.sendMessage(chat_id, "🌡️ Current Temperature: *" + String(g_temp, 1) + " °C*", "Markdown");
    }
    else if (text == "/hum") {
      bot.sendMessage(chat_id, "💧 Current Relative Humidity: *" + String(g_hum, 1) + " %*", "Markdown");
    }
    else if (text == "/air") {
      bot.sendMessage(chat_id, "🌬️ MQ-135 Gas Signal: *" + String(g_ppm, 0) + " ppm* (" + String(mq135QualityLabel(g_ppm)) + ") | Raw: " + String(g_mq135Raw) + "", "Markdown");
    }
    else if (text == "/limits") {
      String lim = "⚙️ *Configured Incident Limits:*\n";
      lim += "• Temperature Limit: *" + String(g_tempLimit, 1) + " °C*\n";
      lim += "• Air Quality Limit: *" + String(g_airLimit) + " ppm*";
      bot.sendMessage(chat_id, lim, "Markdown");
    }
    else if (text == "/simulate") {
      g_demoSimulation = true;
      updateSensors();
      g_dirty = true;
      bot.sendMessage(chat_id, "🧪 *DEMO SIMULATION STARTED*\nSimulated spike active on Dashboard, OLED & Sensors.\nSend `/stop` to restore live DHT22 & MQ-135 data.", "Markdown");
    }
    else if (text == "/stop") {
      g_demoSimulation = false;
      updateSensors();
      g_dirty = true;
      bot.sendMessage(chat_id, "✅ *DEMO SIMULATION STOPPED*\nRestored live DHT22 & MQ-135 sensor telemetry.", "Markdown");
    }
    else if (text == "/test") {
      bot.sendMessage(chat_id, "🔔 *SEMS TEST INCIDENT*\nTelegram notification pipeline verified and fully operational.", "Markdown");
    }
    else {
      bot.sendMessage(chat_id, "📖 *SEMS Bot Help:*\nUse `/status`, `/temp`, `/hum`, `/air`, `/limits`, `/simulate`, `/stop`, or `/test`.", "Markdown");
    }
  }
}

// Non-blocking physical buzzer alert on GPIO 32 (beeps 300ms ON / 300ms OFF when threshold exceeded)
void updateBuzzer() {
  bool alarmActive = (g_temp > g_tempLimit) || (g_ppm > g_airLimit);
  if (!alarmActive) {
    if (g_buzzerBeepOn) {
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      g_buzzerBeepOn = false;
    }
    return;
  }

  uint32_t now = millis();
  if (now - g_lastBuzzerToggleMs >= 300) {
    g_lastBuzzerToggleMs = now;
    g_buzzerBeepOn = !g_buzzerBeepOn;
    if (g_buzzerBeepOn) {
      tone(BUZZER_PIN, 2200);
    } else {
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

// Non-blocking alert check called on Core 1: sets pending flags for Core 0 Telegram task
bool queueTelegramAlerts() {
  unsigned long now = millis();
  bool queued = false;
  if (g_temp > g_tempLimit && (g_lastTempAlertMs == 0 || (now - g_lastTempAlertMs > g_alertCooldownMs))) {
    g_lastTempAlertMs = now;
    g_pendingTempAlert = true;
    queued = true;
  }
  if (g_ppm > g_airLimit && (g_lastAirAlertMs == 0 || (now - g_lastAirAlertMs > g_alertCooldownMs))) {
    g_lastAirAlertMs = now;
    g_pendingAirAlert = true;
    queued = true;
  }
  return queued;
}

bool checkTelegramAlerts() {
  // If Chat ID isn't linked yet, check Telegram once right now in case user already sent /start
  if (g_chatId.length() == 0 && WiFi.status() == WL_CONNECTED) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    if (n > 0) handleNewMessages(n);
  }
  if (g_chatId.length() == 0) return false;

  unsigned long now = millis();
  bool anySent = false;

  // 1. High Temperature Alert (g_lastTempAlertMs == 0 allows immediate alert after boot or threshold change)
  if (g_temp > g_tempLimit && (g_lastTempAlertMs == 0 || (now - g_lastTempAlertMs > g_alertCooldownMs))) {
    g_alertCounter++;
    String alert = "🚨 *SEMS INCIDENT ALERT: HIGH TEMPERATURE*\n";
    if (g_demoSimulation) alert += "🧪 _(Demo Simulation Active)_\n";
    alert += "📍 Node: *ESP32-NODE-01*\n";
    alert += "🌡️ Temperature: *" + String(g_temp, 1) + " °C* (Exceeds limit: " + String(g_tempLimit, 1) + " °C)\n";
    alert += "💧 Relative Humidity: *" + String(g_hum, 1) + " %*\n";
    alert += "⏱️ Timestamp: *" + String(now / 1000) + "s elapsed*\n\n";
    alert += "⚠️ *Action Required:* Inspect cooling, HVAC, or room ventilation.";

    if (sendTelegramSafe(g_chatId, alert)) {
      g_lastTempAlertMs = now;
      anySent = true;
    }
  }

  // 2. Air Contamination Alert (MQ-135 PPM exceeds configured threshold)
  if (g_ppm > g_airLimit && (g_lastAirAlertMs == 0 || (now - g_lastAirAlertMs > g_alertCooldownMs))) {
    g_alertCounter++;
    String alert = "🚨 *SEMS INCIDENT ALERT: AIR CONTAMINATION DETECTED*\n";
    if (g_demoSimulation) alert += "🧪 _(Demo Simulation Active)_\n";
    alert += "📍 Node: *ESP32-NODE-01*\n";
    alert += "🌬️ MQ-135 Signal: *" + String(g_ppm, 0) + " ppm* (" + String(mq135QualityLabel(g_ppm)) + ") | Raw: " + String(g_mq135Raw) + "\n";
    alert += "⚠️ Limit: *" + String(g_airLimit) + " ppm*\n";
    alert += "⏱️ Timestamp: *" + String(now / 1000) + "s elapsed*\n\n";
    alert += "⚠️ *Action Required:* Hazardous gas, smoke, or elevated CO2 detected. Ventilate area.";

    if (sendTelegramSafe(g_chatId, alert)) {
      g_lastAirAlertMs = now;
      anySent = true;
    }
  }
  return anySent;
}

// ======================================================================================
// 6. OLED SCREEN RENDERING
// ======================================================================================
void drawHomeScreen() {
  u8g2.clearBuffer();
  u8g2.setFontPosTop();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Header Bar with Demo / Wi-Fi / Bot Status
  if (g_demoSimulation) {
    u8g2.drawUTF8(10, 2, "SEMS [DEMO SIM]");
  } else if (WiFi.status() == WL_CONNECTED) {
    u8g2.drawUTF8(10, 2, "SEMS - ONLINE (TG)");
  } else {
    u8g2.drawUTF8(18, 2, "SEMS DISPLAY");
  }

  // Temperature / Humidity line
  u8g2.drawXBMP(1, 15, 16, 16, icon_temp_el_mwkfd4o50_xbm);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(17, 19, "TEMP/HUM:");

  char tempHumBuffer[32];
  if (g_dhtSuccess) {
    snprintf(tempHumBuffer, sizeof(tempHumBuffer), "%.1f/%.0f%%", g_temp, g_hum);
  } else {
    snprintf(tempHumBuffer, sizeof(tempHumBuffer), "ERR");
  }
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(75, 19, tempHumBuffer);

  // Gas PPM line
  u8g2.drawXBMP(1, 37, 16, 16, icon_nuke_el_heob4nwqe_xbm);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(17, 39, "Gas PPM:");

  char ppmBuffer[16];
  snprintf(ppmBuffer, sizeof(ppmBuffer), "%.0f", g_ppm);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(67, 39, ppmBuffer);

  // Top header frame & bottom IP display
  u8g2.drawFrame(1, 1, 126, 11);
  char ipBuffer[32];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(ipBuffer, sizeof(ipBuffer), "IP: %s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(ipBuffer, sizeof(ipBuffer), "IP: Offline");
  }
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawUTF8(4, 55, ipBuffer);

  u8g2.sendBuffer();
}

void drawSettingsScreen() {
  u8g2.clearBuffer();
  u8g2.setFontPosTop();
  u8g2.sendBuffer();
}

void drawCurrentScreen() {
  switch (g_screen) {
    case DK_SCREEN_0: drawHomeScreen(); break;
    case DK_SCREEN_1: drawSettingsScreen(); break;
  }
}

void handleUiActions() {
  static int num = -1;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c == 'c' || c == 'C') {
      calibrateR0();
    } else if (c >= '0' && c <= '9') {
      if (num < 0) num = 0;
      num = (num * 10) + (c - '0');
    } else if (c == '\n' || c == '\r') {
      if (num >= 0) {
        const int maxScreen = 1;
        if (num > maxScreen) num = maxScreen;
        const DkScreenId next = (DkScreenId)num;
        if (next != g_screen) { g_screen = next; g_dirty = true; }
      }
      num = -1;
    }
  }
}

// ======================================================================================
// 7. WEB SERVER & REST API (FULL INDEX.HTML INTEGRATION)
// ======================================================================================
void setCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

// GET / -> Serve the FULL index.html dashboard directly from ESP32 PROGMEM
void handleRoot() {
  setCorsHeaders();
  server.send_P(200, "text/html", INDEX_HTML);
}

// GET /sems-logo.jpg -> Serve embedded SEMS logo from PROGMEM
void handleLogo() {
  setCorsHeaders();
  server.send_P(200, "image/jpeg", (PGM_P)SEMS_LOGO_JPG, sizeof(SEMS_LOGO_JPG));
}

// GET /api/data -> Real-time JSON telemetry stream for index.html
void handleApiData() {
  setCorsHeaders();
  String json = "{";
  json += "\"temperature\":" + String(g_temp, 1) + ",";
  json += "\"humidity\":" + String(g_hum, 1) + ",";
  json += "\"air_adc\":" + String(g_mq135Raw) + ",";
  json += "\"air_ppm\":" + String(g_ppm, 1) + ",";
  json += "\"mq_voltage\":" + String(g_mq135Voltage, 3) + ",";
  json += "\"mq_rs\":" + String(g_mq135Rs, 2) + ",";
  json += "\"mq_ratio\":" + String(g_mq135Ratio, 3) + ",";
  json += "\"air_quality\":\"" + String(mq135QualityLabel(g_ppm)) + "\",";
  json += "\"dht_ok\":" + String(g_dhtSuccess ? "true" : "false") + ",";
  json += "\"simulation\":" + String(g_demoSimulation ? "true" : "false") + ",";
  json += "\"temp_limit\":" + String(g_tempLimit, 1) + ",";
  json += "\"air_limit\":" + String(g_airLimit) + ",";
  json += "\"alert_count\":" + String(g_alertCounter) + ",";
  json += "\"packet_count\":" + String(g_packetCounter) + ",";
  json += "\"uptime_sec\":" + String((millis() - g_bootTimeMs) / 1000) + ",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"bot_username\":\"SemibotSys_Bot\",";
  json += "\"chat_id_set\":" + String(g_chatId.length() > 0 ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// POST or GET /api/thresholds?temp=30.0&air=1000 -> Synchronize limits from index.html & trigger immediate check
void handleSetThresholds() {
  setCorsHeaders();
  if (server.hasArg("temp")) g_tempLimit = server.arg("temp").toFloat();
  if (server.hasArg("air"))  g_airLimit  = constrain(server.arg("air").toInt(), 0, 5000);

  g_lastTempAlertMs = 0;
  g_lastAirAlertMs  = 0;
  Serial.printf("[CONFIG] Thresholds updated from web -> Temp: %.1f C | Air: %d ppm\n", g_tempLimit, g_airLimit);

  bool queued = queueTelegramAlerts();
  bool tgSent = queued && (g_chatId.length() > 0);

  bool tempExceeded = (g_temp > g_tempLimit);
  bool airExceeded  = (g_ppm > g_airLimit);
  String resp = "{\"status\":\"ok\",\"temp_alert\":" + String(tempExceeded ? "true" : "false") +
                ",\"air_alert\":" + String(airExceeded ? "true" : "false") +
                ",\"telegram_sent\":" + String(tgSent ? "true" : "false") +
                ",\"chat_linked\":" + String(g_chatId.length() > 0 ? "true" : "false") + "}";
  server.send(200, "application/json", resp);
}

// POST or GET /api/test_alert -> Trigger real Telegram notification from index.html button
void handleApiTestAlert() {
  setCorsHeaders();
  bool hasChat = (g_chatId.length() > 0);
  g_pendingTestAlert = true;
  server.send(200, "application/json", "{\"status\":\"" + String(hasChat ? "sent" : "failed_no_chat_id") + "\"}");
}

// POST or GET /api/simulate?active=1 -> Trigger or stop Demo Simulation from index.html
void handleApiSimulate() {
  setCorsHeaders();
  if (server.hasArg("active")) {
    g_demoSimulation = (server.arg("active") == "1" || server.arg("active") == "true");
  } else {
    g_demoSimulation = !g_demoSimulation;
  }

  updateSensors();
  drawCurrentScreen();
  g_dirty = true;

  bool tgSent = false;
  if (g_demoSimulation) {
    g_alertCounter++;
    g_lastTempAlertMs = millis();
    g_lastAirAlertMs = millis();
    g_pendingSimAlert = true;
    tgSent = (g_chatId.length() > 0);
  }

  String resp = "{\"simulation\":" + String(g_demoSimulation ? "true" : "false") +
                ",\"telegram_sent\":" + String(tgSent ? "true" : "false") + "}";
  server.send(200, "application/json", resp);
}

// Core 0 background task: handles all blocking HTTPS calls to Telegram so Core 1 WebServer stays instant (<5ms)
void telegramTask(void* param) {
  uint32_t lastPollMs = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (g_pendingTestAlert) {
        g_pendingTestAlert = false;
        if (g_chatId.length() == 0) {
          int n = bot.getUpdates(bot.last_message_received + 1);
          if (n > 0) handleNewMessages(n);
        }
        sendTelegramSafe(g_chatId, "🔔 *SEMS Operator Test Alert*\nNotification verified from web dashboard via @SemibotSys_Bot.");
      }

      if (g_pendingSimAlert) {
        g_pendingSimAlert = false;
        if (g_chatId.length() == 0) {
          int n = bot.getUpdates(bot.last_message_received + 1);
          if (n > 0) handleNewMessages(n);
        }
        if (g_chatId.length() > 0) {
          String msg = "🧪 *SEMS DEMO SIMULATION TRIGGERED*\n";
          msg += "📍 Node: *ESP32-NODE-01*\n";
          msg += "🌡️ Simulated Temp: *" + String(g_temp, 1) + " °C* (Limit: " + String(g_tempLimit, 1) + " °C)\n";
          msg += "🌬️ Simulated MQ-135: *" + String(g_ppm, 0) + " ppm* (" + String(mq135QualityLabel(g_ppm)) + ") | Raw: " + String(g_mq135Raw) + "\n\n";
          msg += "_Triggered from SEMS Web Dashboard for demo purposes._";
          sendTelegramSafe(g_chatId, msg);
        }
      }

      if (g_pendingTempAlert) {
        g_pendingTempAlert = false;
        g_lastTempAlertMs = 0;
        checkTelegramAlerts();
      }
      if (g_pendingAirAlert) {
        g_pendingAirAlert = false;
        g_lastAirAlertMs = 0;
        checkTelegramAlerts();
      }

      uint32_t now = millis();
      if (now - lastPollMs >= 4000) {
        lastPollMs = now;
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        while (numNewMessages) {
          handleNewMessages(numNewMessages);
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup() {
  Serial.begin(115200);

  // Setup DHT & MQ-135 Pins
  dht.begin();
  pinMode(MQ135_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);

  // Setup Physical Alert Buzzer (GPIO 32)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Setup OLED Display
  u8g2.begin();
  u8g2.setFontMode(0);
  u8g2.setPowerSave(0);

  // Configure Secure Client for Telegram Bot & load saved Chat ID from NVS
  secured_client.setInsecure();
  prefs.begin("sems", true);
  g_chatId = prefs.getString("chat_id", "");
  prefs.end();
  if (g_chatId.length() > 0) {
    Serial.printf("[TELEGRAM] Loaded saved Chat ID from NVS: %s\n", g_chatId.c_str());
  }

  // Connect to Wi-Fi
  Serial.printf("\n[WIFI] Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WIFI] Connected successfully!"));
    Serial.print(F("[WIFI] IP Address: "));
    Serial.println(WiFi.localIP());

    if (MDNS.begin(MDNS_NAME)) {
      Serial.printf("[MDNS] Responder active: http://%s.local\n", MDNS_NAME);
    }
  } else {
    Serial.println(F("\n[WIFI] Warning: Connection timed out. Running in offline/reconnect mode."));
  }

  // Register Web Server Routes
  server.on("/", handleRoot);
  server.on("/index.html", handleRoot);
  server.on("/index2.html", handleRoot);
  server.on("/sems-logo.jpg", handleLogo);
  server.on("/api/data", handleApiData);
  server.on("/api/thresholds", handleSetThresholds);
  server.on("/api/test_alert", handleApiTestAlert);
  server.on("/api/simulate", handleApiSimulate);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      setCorsHeaders();
      server.send(204);
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
  server.begin();
  Serial.println(F("[HTTP] Full SEMS Web Dashboard & REST API listening on port 80"));

  // MQ-135 wiring check + initial sensor read & display
  checkMq135Wiring();
  Serial.println(F("[MQ-135] Send 'c' in Serial Monitor to calibrate R0 in clean air."));
  updateSensors();
  drawCurrentScreen();

  g_bootTimeMs = millis();
  g_dirty = false;
  g_lastDrawMs = millis();
  g_lastMqReadMs = millis();

  // Launch Telegram HTTPS task on Core 0 so Core 1 WebServer responds in <5ms
  xTaskCreatePinnedToCore(telegramTask, "TelegramTask", 8192, NULL, 1, NULL, 0);

  Serial.println(F("[TELEGRAM] Bot @SemibotSys_Bot is ready (Core 0 task). Send /start to link your chat!"));
}

void loop() {
  handleUiActions();
  server.handleClient();

  unsigned long currentMs = millis();

  // 1. Fast live sensor loop (MQ-135 every 500ms, DHT22 every 2000ms)
  if (currentMs - g_lastMqReadMs >= 500) {
    updateSensors();
    queueTelegramAlerts();
    g_lastMqReadMs = currentMs;
    g_dirty = true;
  }

  // 2. Physical buzzer alert on GPIO 32 when readings exceed thresholds
  updateBuzzer();

  // 2. Redraw OLED display when requested
  if (g_dirty || (currentMs - g_lastDrawMs) > 500) {
    drawCurrentScreen();
    g_dirty = false;
    g_lastDrawMs = currentMs;
  }

  // 3. Auto-reconnect Wi-Fi if dropped
  if (WiFi.status() != WL_CONNECTED && (currentMs % 10000 < 50)) {
    Serial.println(F("[WIFI] Attempting reconnect..."));
    WiFi.reconnect();
  }

  delay(2);
}

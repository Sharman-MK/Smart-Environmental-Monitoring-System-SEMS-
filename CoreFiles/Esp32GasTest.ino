/*
 * ======================================================================================
 *  SEMS - MQ-135 Gas Sensor Test (ESP32)
 * ======================================================================================
 *  Purpose : Standalone test for the MQ-135 gas / air-quality sensor before running
 *            the full SEMS_ESP32.ino firmware.
 *
 *  Wiring (same as SEMS_ESP32.ino):
 *    MQ-135 VCC  -> 5V (VIN)      (heater needs 5V)
 *    MQ-135 GND  -> GND
 *    MQ-135 AOUT -> GPIO 36 (VP / ADC1_CH0)
 *    MQ-135 DOUT -> GPIO 27 (optional, digital threshold set by the module's trimpot)
 *
 *  NOTE: ESP32 ADC pins accept max 3.3V. If your module is powered at 5V, AOUT can go
 *        above 3.3V. Use a voltage divider (e.g. 10k / 20k) and set DIVIDER_RATIO below.
 *
 *  Serial Monitor: 115200 baud, line ending "Newline"
 *  Commands:
 *    c  -> Calibrate R0 in clean/fresh air (prints value to paste into SEMS_ESP32.ino)
 *    p  -> Toggle Serial Plotter mode (numbers only)
 *    s  -> Skip the warm-up countdown
 *    h  -> Show help
 * ======================================================================================
 */

#include <Arduino.h>
#include <math.h>

// ---------------------------------- PIN SETUP ----------------------------------------
#define MQ135_PIN       36      // Analog output (ADC1 - safe to use with Wi-Fi)
#define MQ135_DOUT_PIN  27      // Digital output (optional)
#define USE_DOUT        false   // Set true if DOUT is wired

// ------------------------- SENSOR CONSTANTS (match SEMS) -----------------------------
#define RL_VALUE        10.0f   // Load resistor on the module (kOhm)
float   R0_CLEAN_AIR =  76.63f; // Baseline resistance in clean air (kOhm) - calibrate with 'c'
const float GAS_A = 110.47f;    // CO2 curve scaling factor
const float GAS_B = -2.862f;    // CO2 curve exponent
const float ATMOSPHERIC_CO2_BASELINE = 400.0f;

const float ADC_VREF      = 3.3f;  // ESP32 ADC reference / sensor circuit voltage
const float DIVIDER_RATIO = 1.0f;  // AOUT -> ADC divider ratio (1.0 = no divider, 0.667 = 10k/20k)

// ------------------------------- TEST SETTINGS ---------------------------------------
const uint16_t SAMPLES          = 20;     // Samples averaged per reading
const uint16_t SAMPLE_DELAY_MS  = 5;      // Delay between samples
const uint32_t READ_INTERVAL_MS = 1000;   // Print interval
const uint32_t WARMUP_SECONDS   = 60;     // Short warm-up (24h burn-in recommended for new sensors)
const uint16_t CALIB_SAMPLES    = 50;     // Readings averaged during calibration

// ------------------------------------ STATE ------------------------------------------
bool     g_plotterMode = false;
bool     g_warmupDone  = false;
uint32_t g_bootMs      = 0;
uint32_t g_lastReadMs  = 0;
float    g_minPpm = 99999.0f, g_maxPpm = 0.0f;

// ================================ HELPER FUNCTIONS ===================================

// Averaged raw ADC reading (0 - 4095)
int readRawAveraged() {
  long sum = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) {
    sum += analogRead(MQ135_PIN);
    delay(SAMPLE_DELAY_MS);
  }
  return (int)(sum / SAMPLES);
}

// Averaged calibrated voltage at the ADC pin (uses ESP32 factory eFuse calibration)
float readVoltageAveraged() {
  uint32_t sumMv = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) {
    sumMv += analogReadMilliVolts(MQ135_PIN);
    delay(SAMPLE_DELAY_MS);
  }
  float vAdc = (sumMv / (float)SAMPLES) / 1000.0f;
  return vAdc / DIVIDER_RATIO;   // Voltage at the sensor AOUT
}

// Sensor resistance Rs (kOhm)
float calculateRs(float vOut) {
  if (vOut <= 0.01f) return -1.0f;           // Sensor not connected / no signal
  float vc = ADC_VREF / DIVIDER_RATIO;       // Circuit supply as seen by the sensor
  return RL_VALUE * (vc - vOut) / vOut;
}

// Gas concentration (ppm, CO2-equivalent) - same formula as SEMS_ESP32.ino
float calculatePPM(float rs) {
  if (rs <= 0) return ATMOSPHERIC_CO2_BASELINE;
  float ratio = rs / R0_CLEAN_AIR;
  if (ratio <= 0) return ATMOSPHERIC_CO2_BASELINE;
  float ppm = (GAS_A * pow(ratio, GAS_B)) + ATMOSPHERIC_CO2_BASELINE;
  return constrain(ppm, 350.0f, 5000.0f);
}

const char* airQualityLabel(float ppm) {
  if (ppm < 600)  return "GOOD";
  if (ppm < 1000) return "MODERATE";
  if (ppm < 2000) return "POOR";
  return "HAZARDOUS";
}

void printHelp() {
  Serial.println(F("\n---------------- MQ-135 TEST COMMANDS ----------------"));
  Serial.println(F("  c  -> Calibrate R0 (place sensor in fresh/clean air)"));
  Serial.println(F("  p  -> Toggle Serial Plotter mode"));
  Serial.println(F("  s  -> Skip warm-up"));
  Serial.println(F("  h  -> Show this help"));
  Serial.println(F("------------------------------------------------------\n"));
}

// Wiring sanity check at boot
void checkWiring() {
  int raw = readRawAveraged();
  Serial.printf("[CHECK] Initial raw ADC = %d\n", raw);
  if (raw <= 10) {
    Serial.println(F("[WARN] ADC ~0 -> AOUT not connected, wrong pin, or sensor has no power."));
  } else if (raw >= 4085) {
    Serial.println(F("[WARN] ADC saturated (4095) -> AOUT above 3.3V. Add a voltage divider!"));
  } else {
    Serial.println(F("[OK] MQ-135 analog signal detected."));
  }
}

// R0 calibration in clean air
void calibrateR0() {
  Serial.println(F("\n[CALIBRATION] Keep the sensor in fresh/clean air..."));
  float sumRs = 0;
  uint16_t valid = 0;
  for (uint16_t i = 0; i < CALIB_SAMPLES; i++) {
    float rs = calculateRs(readVoltageAveraged());
    if (rs > 0) { sumRs += rs; valid++; }
    if (i % 10 == 0) Serial.printf("  progress: %u/%u\n", i, CALIB_SAMPLES);
    delay(100);
  }
  if (valid == 0) {
    Serial.println(F("[CALIBRATION] FAILED - no valid signal. Check wiring."));
    return;
  }
  R0_CLEAN_AIR = sumRs / valid;
  g_minPpm = 99999.0f; g_maxPpm = 0.0f;
  Serial.printf("[CALIBRATION] Done. New R0 = %.2f kOhm\n", R0_CLEAN_AIR);
  Serial.printf("  -> Paste into SEMS_ESP32.ino:  #define R0_CLEAN_AIR  %.2ff\n\n", R0_CLEAN_AIR);
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = tolower(Serial.read());
    switch (c) {
      case 'c': calibrateR0(); break;
      case 'p':
        g_plotterMode = !g_plotterMode;
        if (!g_plotterMode) Serial.println(F("\n[MODE] Serial Monitor text mode"));
        break;
      case 's':
        if (!g_warmupDone) { g_warmupDone = true; Serial.println(F("[WARM-UP] Skipped.")); }
        break;
      case 'h': printHelp(); break;
      default: break;  // Ignore newline / other chars
    }
  }
}

// ===================================== SETUP =========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);                       // 0 - 4095
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);   // ~0 - 3.3V input range
  pinMode(MQ135_PIN, INPUT);
  if (USE_DOUT) pinMode(MQ135_DOUT_PIN, INPUT);

  Serial.println(F("\n======================================================"));
  Serial.println(F("        SEMS - MQ-135 GAS SENSOR TEST (ESP32)"));
  Serial.println(F("======================================================"));
  Serial.printf("AOUT pin: GPIO %d | RL: %.1f kOhm | R0: %.2f kOhm\n", MQ135_PIN, RL_VALUE, R0_CLEAN_AIR);
  printHelp();
  checkWiring();

  Serial.printf("[WARM-UP] Heating sensor for %lu s (send 's' to skip)...\n", (unsigned long)WARMUP_SECONDS);
  g_bootMs = millis();
}

// ====================================== LOOP =========================================
void loop() {
  handleSerialCommands();

  uint32_t now = millis();

  // Warm-up countdown
  if (!g_warmupDone) {
    uint32_t elapsed = (now - g_bootMs) / 1000;
    if (elapsed >= WARMUP_SECONDS) {
      g_warmupDone = true;
      Serial.println(F("[WARM-UP] Complete. Starting measurements.\n"));
    } else if (now - g_lastReadMs >= 5000) {
      g_lastReadMs = now;
      Serial.printf("[WARM-UP] %lu s remaining | raw ADC: %d\n",
                    (unsigned long)(WARMUP_SECONDS - elapsed), readRawAveraged());
    }
    return;
  }

  if (now - g_lastReadMs < READ_INTERVAL_MS) return;
  g_lastReadMs = now;

  // ---- Read sensor ----
  int   raw   = readRawAveraged();
  float vOut  = readVoltageAveraged();
  float rs    = calculateRs(vOut);
  float ratio = (rs > 0) ? rs / R0_CLEAN_AIR : 0.0f;
  float ppm   = calculatePPM(rs);

  if (rs > 0) {
    g_minPpm = min(g_minPpm, ppm);
    g_maxPpm = max(g_maxPpm, ppm);
  }

  // ---- Output ----
  if (g_plotterMode) {
    // Arduino Serial Plotter format
    Serial.printf("RawADC:%d,PPM:%.0f,Rs_R0x100:%.0f\n", raw, ppm, ratio * 100.0f);
    return;
  }

  if (rs <= 0) {
    Serial.printf("[MQ-135] Raw: %4d | Voltage: %.3f V | NO SIGNAL - check wiring/power\n", raw, vOut);
    return;
  }

  Serial.printf("[MQ-135] Raw: %4d | V: %.3f V | Rs: %6.2f kOhm | Rs/R0: %.3f | Gas: %6.0f ppm | %-9s | Min/Max: %.0f/%.0f",
                raw, vOut, rs, ratio, ppm, airQualityLabel(ppm), g_minPpm, g_maxPpm);

  if (USE_DOUT) {
    // Most modules pull DOUT LOW when gas exceeds the trimpot threshold
    Serial.printf(" | DOUT: %s", digitalRead(MQ135_DOUT_PIN) == LOW ? "GAS DETECTED" : "normal");
  }
  Serial.println();
}

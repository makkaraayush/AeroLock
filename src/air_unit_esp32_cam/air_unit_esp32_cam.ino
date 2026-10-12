/*
  Air Unit Firmware (Mounted on Drone)
  Hardware: AI-Thinker ESP32-CAM (OV2640) + LoRa Ra-02 (SX1278 433MHz)
  Connections: 
    - Hardware Serial (U0R GPIO 3, U0T GPIO 1) connected to F405 FC UART at 19200 baud.
    - Ra-02 LoRa connected over SPI on Header 1 (SCK 13, MISO 15, MOSI 14, SS 2, RST 12).
    - Ra-02 3.3V power strictly sourced from ESP32-CAM 3V3 output pin.
    - Flash LED on GPIO 4 (PWM brightness controlled via Web UI).

  Dual-Server & Dual-Core Architecture:
    - Core 0: Low-latency MAVLink UART <-> LoRa 433 MHz RF bridge (FreeRTOS pinned task).
    - Core 1: 
        * Port 80 (camera_httpd): Dedicated to UI & real-time controls (0ms latency, never blocks).
        * Port 81 (stream_httpd): Dedicated continuous MJPEG video stream worker.
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <SPI.h>
#include <LoRa.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <WiFiUdp.h>
#include "esp_http_server.h"

// =================== PIN DEFINITIONS ===================
// LoRa SPI Bus (Straight 4-wire physical ribbon: SCK, MISO, MOSI, NSS + RST)
#define LORA_SCK       13      // SPI Clock (Header 1, Pin 4)
#define LORA_MISO      15      // SPI Master In Slave Out (Header 1, Pin 5)
#define LORA_MOSI      14      // SPI Master Out Slave In (Header 1, Pin 6)
#define LORA_SS        2       // SPI Chip Select (Header 1, Pin 7)
#define LORA_RST       12      // Hardware Reset pin (Header 1, Pin 3)
#define LORA_DIO0      -1

// Onboard High-Power Flash LED
#define FLASH_LED_PIN  4

// AI-Thinker OV2640 Camera Pin Map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Wi-Fi Access Point Credentials
const char* AP_SSID = "BuddyThisAintFreeWifi";
const char* AP_PASS = "notfree123";

// Runtime Camera & Flash States
int currentFlashDuty  = 0;
int currentFramesize  = FRAMESIZE_QVGA; // 5 = 320x240 (Fastest, low latency)
int currentQuality    = 12;             // 10-63 scale
int currentBrightness = 0;
int currentContrast   = 0;
int currentSaturation = 0;
int currentVflip      = 0;
int currentHmirror    = 0;
int currentEffect     = 0;
volatile bool loraReady = false;
volatile uint32_t fcByteCount = 0;
volatile uint32_t loraPktCount = 0;

// Decoded MAVLink Telemetry States (displayed in Web Cockpit OSD)
char flightMode[16] = "STABILIZE";
volatile bool droneArmed = false;
volatile float droneBatVolts = 0.0;
volatile int droneBatPct = 0;
volatile float droneAltitude = 0.0;
volatile int droneSats = 0;
volatile float droneSpeed = 0.0;
volatile unsigned long lastMavlinkTime = 0;
volatile bool fastFlushRequested = false; // Fast-forward mode changes and emergency alerts over LoRa

// =================== LIVE DIAGNOSTIC LOG BUFFER ===================
#define MAX_LOG_LINES 30
static char logBuffer[MAX_LOG_LINES][100];
static uint8_t logHead = 0;
static uint8_t logCount = 0;

void addLog(const char* fmt, ...) {
  char temp[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(temp, sizeof(temp), fmt, args);
  va_end(args);

  snprintf(logBuffer[logHead], sizeof(logBuffer[logHead]), "[%06lu] %s", millis() / 1000, temp);
  logHead = (logHead + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
}

// =================== MAVLINK UDP BRIDGE (QGroundControl on Wi-Fi Port 14550) ===================
WiFiUDP mavUdp;
IPAddress qgcClientIp(0, 0, 0, 0);
uint16_t qgcClientPort = 14550;
bool qgcConnectedViaWifi = false;
unsigned long lastQgcUdpPacketTime = 0;

httpd_handle_t camera_httpd = NULL; // Port 80 (UI & Real-time Controls)
httpd_handle_t stream_httpd = NULL; // Port 81 (Dedicated Stream Worker)
TaskHandle_t LoRaBridgeTask;

// =================== FLASH LED PWM CONTROL ===================
void setFlashBrightness(int duty) {
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  currentFlashDuty = duty;

  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcWrite(FLASH_LED_PIN, duty);
  #else
    ledcWrite(7, duty);
  #endif
}

void initFlash() {
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW); // Start with flash completely OFF
  
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttach(FLASH_LED_PIN, 5000, 8);
    ledcWrite(FLASH_LED_PIN, 0);
  #else
    ledcSetup(7, 5000, 8);
    ledcAttachPin(FLASH_LED_PIN, 7);
    ledcWrite(7, 0);
  #endif
}

// =================== AIR-GROUND DIAGNOSTIC PACKET ===================
// 13-byte lightweight frame sent at 1 Hz from Air Unit to Ground Station
struct __attribute__((packed)) AirDiagPacket {
  uint8_t header[3];     // "$AG" -> { 0x24, 0x41, 0x47 }
  uint32_t fcByteCount;  // Total bytes received from F405 FC UART
  uint32_t loraPktCount; // Total LoRa packets transmitted
  uint8_t flags;         // Status flags (bit 0 = radio ready)
  uint8_t checksum;      // XOR checksum across preceding 12 bytes
};

static uint8_t calcChecksum(const uint8_t* data, size_t len) {
  uint8_t cs = 0;
  for (size_t i = 0; i < len; i++) {
    cs ^= data[i];
  }
  return cs;
}

// =================== AIR UNIT MAVLINK PARSER ===================
// Extracts flight mode, battery, altitude, sats, and speed for Web Cockpit OSD
void parseAirMavlinkByte(uint8_t c) {
  static uint8_t state = 0;
  static uint8_t payloadLen = 0;
  static uint32_t msgId = 0;
  static uint8_t buf[64];
  static uint16_t bytesRead = 0;
  static unsigned long lastByteTime = 0;

  if (state != 0 && (millis() - lastByteTime > 150)) {
    state = 0;
  }
  lastByteTime = millis();

  switch (state) {
    case 0:
      if (c == 0xFD) state = 1;       // MAVLink 2
      else if (c == 0xFE) state = 10; // MAVLink 1
      break;

    // MAVLink 2
    case 1: payloadLen = c; state = 2; break;
    case 2: state = 3; break;
    case 3: state = 4; break;
    case 4: state = 5; break;
    case 5: state = 6; break;
    case 6: state = 7; break;
    case 7: msgId = c; state = 8; break;
    case 8: msgId |= ((uint32_t)c << 8); state = 9; break;
    case 9: msgId |= ((uint32_t)c << 16); state = 20; bytesRead = 0; break;

    // MAVLink 1
    case 10: payloadLen = c; state = 11; break;
    case 11: state = 12; break;
    case 12: state = 13; break;
    case 13: state = 14; break;
    case 14: msgId = c; state = 20; bytesRead = 0; break;

    // Payload
    case 20:
      if (bytesRead < sizeof(buf)) {
        buf[bytesRead] = c;
      }
      bytesRead++;

      if (bytesRead >= payloadLen) {
        lastMavlinkTime = millis();

        // 1. HEARTBEAT (Msg #0)
        if (msgId == 0 && payloadLen >= 9) {
          uint32_t customMode = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
          bool newArmed = (buf[6] & 128) != 0;
          droneArmed = newArmed;

          // Decode all ArduCopter flight modes
          switch (customMode) {
            case 0:  strcpy(flightMode, "STABILIZE"); break;
            case 1:  strcpy(flightMode, "ACRO");      break;
            case 2:  strcpy(flightMode, "ALT_HOLD");  break;
            case 3:  strcpy(flightMode, "AUTO");      break;
            case 4:  strcpy(flightMode, "GUIDED");    break;
            case 5:  strcpy(flightMode, "LOITER");    break;
            case 6:  strcpy(flightMode, "RTL");       break;
            case 7:  strcpy(flightMode, "CIRCLE");    break;
            case 9:  strcpy(flightMode, "LAND");      break;
            case 11: strcpy(flightMode, "DRIFT");     break;
            case 13: strcpy(flightMode, "SPORT");     break;
            case 14: strcpy(flightMode, "FLIP");      break;
            case 15: strcpy(flightMode, "AUTOTUNE");  break;
            case 16: strcpy(flightMode, "POSHOLD");   break;
            case 17: strcpy(flightMode, "BRAKE");     break;
            case 18: strcpy(flightMode, "THROW");     break;
            case 20: strcpy(flightMode, "GUIDED_NOGPS"); break;
            case 21: strcpy(flightMode, "SMART_RTL"); break;
            case 22: strcpy(flightMode, "FLOWHOLD");  break;
            case 23: strcpy(flightMode, "FOLLOW");    break;
            case 24: strcpy(flightMode, "ZIGZAG");    break;
            case 27: strcpy(flightMode, "AUTO_RTL");  break;
            default: break; // NEVER overwrite valid flight mode with "CUSTOM"!
          }

          static uint32_t lastAirMode = 0xFFFFFFFF;
          static bool lastAirArmed = false;
          if (customMode != lastAirMode || newArmed != lastAirArmed) {
            lastAirMode = customMode;
            lastAirArmed = newArmed;
            fastFlushRequested = true; // RC Remote mode or arm status changed -> Immediate LoRa TX!
            addLog("FC MODE: %s [Armed=%d]", flightMode, newArmed ? 1 : 0);
          }
        }
        // 2. SYS_STATUS (Msg #1)
        else if (msgId == 1 && payloadLen >= 31) {
          uint16_t mVolts = buf[14] | (buf[15] << 8);
          droneBatVolts = mVolts / 1000.0;
          droneBatPct = constrain((int)((droneBatVolts - 10.5) / (12.6 - 10.5) * 100.0), 0, 100);
        }
        // 3. GLOBAL_POSITION_INT (Msg #33)
        else if (msgId == 33 && payloadLen >= 28) {
          int32_t relAltMm = (int32_t)(buf[16] | (buf[17] << 8) | (buf[18] << 16) | (buf[19] << 24));
          droneAltitude = relAltMm / 1000.0;
          int16_t vx = (int16_t)(buf[20] | (buf[21] << 8));
          int16_t vy = (int16_t)(buf[22] | (buf[23] << 8));
          droneSpeed = (sqrt((float)vx*vx + (float)vy*vy) / 100.0) * 3.6; // km/h
        }
        // 4. GPS_RAW_INT (Msg #24)
        else if (msgId == 24 && payloadLen >= 30) {
          droneSats = buf[29];
        }
        // 5. STATUSTEXT (Msg #253) - Flight mode announcements & warning toasts
        else if (msgId == 253) {
          fastFlushRequested = true; // Send mode change notifications instantly to QGC
          char tBuf[51];
          int tLen = (payloadLen > 1) ? payloadLen - 1 : 0;
          if (tLen > 50) tLen = 50;
          memcpy(tBuf, (char*)&buf[1], tLen);
          tBuf[tLen] = '\0';
          addLog("FC TOAST: %s", tBuf);

          // Instant flight mode extraction from "Mode <MODE_NAME>" (0ms delay!)
          char* mPtr = strstr(tBuf, "Mode ");
          if (mPtr != NULL) {
            char* mName = mPtr + 5;
            while (*mName == ' ') mName++;
            if (strlen(mName) >= 3) {
              char cleanMode[16];
              int idx = 0;
              while (mName[idx] && mName[idx] != ' ' && mName[idx] != '\r' && mName[idx] != '\n' && idx < 15) {
                char ch = mName[idx];
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                cleanMode[idx++] = ch;
              }
              cleanMode[idx] = '\0';
              if (idx >= 3) {
                strcpy(flightMode, cleanMode);
                addLog("INSTANT MODE: %s", flightMode);
              }
            }
          }
        }
        // 6. COMMAND_ACK (Msg #77) - Critical Arm / Disarm / Mode / Takeoff acknowledgment
        else if (msgId == 77) {
          fastFlushRequested = true; // Flush COMMAND_ACK immediately so QGC gets confirmation within 100ms!
          uint16_t cmd = buf[0] | (buf[1] << 8);
          uint8_t res = (payloadLen >= 3) ? buf[2] : 255;
          addLog("FC ACK: Msg #77 Cmd %u, Res %u", cmd, res);
        }
        // 7. Mission negotiation: MISSION_ACK (47), MISSION_REQUEST (40), MISSION_COUNT (44)
        else if (msgId == 47 || msgId == 40 || msgId == 44) {
          fastFlushRequested = true; // Fast-forward waypoint mission negotiation
        }

        state = 30;
      }
      break;

    case 30: state = 31; break;
    case 31: state = 0; break;
  }
}

// =================== HARDWARE-SAFE NON-BLOCKING LORA DRIVER ===================
static uint8_t readLoRaReg(uint8_t addr) {
  digitalWrite(LORA_SS, LOW);
  SPI.beginTransaction(SPISettings(8E6, MSBFIRST, SPI_MODE0));
  SPI.transfer(addr & 0x7F);
  uint8_t val = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(LORA_SS, HIGH);
  return val;
}

static void writeLoRaReg(uint8_t addr, uint8_t val) {
  digitalWrite(LORA_SS, LOW);
  SPI.beginTransaction(SPISettings(8E6, MSBFIRST, SPI_MODE0));
  SPI.transfer(addr | 0x80);
  SPI.transfer(val);
  SPI.endTransaction();
  digitalWrite(LORA_SS, HIGH);
}

void recoverLoRaRadio() {
  if (LORA_RST != -1) {
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(10);
    digitalWrite(LORA_RST, HIGH);
    delay(10);
  }
  if (LoRa.begin(433E6)) {
    loraReady = true;
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(250E3);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(0x12);
    LoRa.setTxPower(17); // +17dBm (50mW) prevents 3.3V rail voltage sag during motor load/RTL
    LoRa.setOCP(100);    // 100mA overcurrent limit protects ESP32-CAM 3V3 rail
  } else {
    loraReady = false;
  }
}

bool safeLoRaSend(const uint8_t* data, size_t len, uint32_t timeoutMs = 120) {
  if (!loraReady || len == 0) return false;

  if (!LoRa.beginPacket()) {
    recoverLoRaRadio();
    return false;
  }
  LoRa.write(data, len);
  LoRa.endPacket(true); // Non-blocking async start - NEVER enters infinite while loop!

  unsigned long start = millis();
  bool txDone = false;
  while (millis() - start < timeoutMs) {
    uint8_t flags = readLoRaReg(0x12); // REG_IRQ_FLAGS
    if (flags & 0x08) { // IRQ_TX_DONE_MASK
      writeLoRaReg(0x12, 0x08); // Clear TX_DONE flag
      txDone = true;
      break;
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }

  if (!txDone) {
    // Radio timed out or PLL brownout occurred during motor load/RTL -> auto-recover
    recoverLoRaRadio();
    return false;
  }

  // Put radio into RX mode immediately
  LoRa.parsePacket();
  return true;
}

// =================== CORE 0: MAVLINK <-> LORA BRIDGE ===================
void loraBridgeLoop(void * pvParameters) {
  static uint8_t stagingBuf[160];
  static size_t stagingLen = 0;
  static unsigned long lastStagingTime = 0;
  static unsigned long lastAirDiagTime = 0;
  
  for(;;) {
    // 0. Auto-reconnect if Ra-02 was powered on late or reattached
    if (!loraReady) {
      static unsigned long lastRetry = 0;
      if (millis() - lastRetry > 2000) {
        lastRetry = millis();
        recoverLoRaRadio();
      }
    }

    // 1. Read MAVLink bytes from F405 flight controller -> ALWAYS PARSE TELEMETRY UNCONDITIONALLY!
    while (Serial.available()) {
      uint8_t b = Serial.read();
      fcByteCount++;
      parseAirMavlinkByte(b); // Local Web Cockpit HUD
      if (stagingLen < sizeof(stagingBuf)) {
        stagingBuf[stagingLen++] = b;
      }
      lastStagingTime = millis();
    }

    // 2. Transmit telemetry packet: Fast-flush if mode changed, or when buffer filled, or after quiet gap
    if ((fastFlushRequested && stagingLen > 0) || stagingLen >= 120 || (stagingLen > 0 && (millis() - lastStagingTime >= 15))) {
      fastFlushRequested = false;

      // Forward telemetry to QGroundControl over Wi-Fi UDP (Port 14550) FIRST (instant <0.1ms!)
      if (WiFi.softAPgetStationNum() > 0) {
        if (qgcConnectedViaWifi && (millis() - lastQgcUdpPacketTime < 10000)) {
          mavUdp.beginPacket(qgcClientIp, qgcClientPort);
          mavUdp.write(stagingBuf, stagingLen);
          mavUdp.endPacket();
        } else {
          mavUdp.beginPacket(IPAddress(192, 168, 4, 255), 14550);
          mavUdp.write(stagingBuf, stagingLen);
          mavUdp.endPacket();
        }
      }

      // Transmit telemetry over LoRa (SX1278) for Ground Station
      if (loraReady) {
        if (safeLoRaSend(stagingBuf, stagingLen)) {
          loraPktCount++;
        }
      }

      stagingLen = 0;

      // 3. Dedicated Uplink Receive Window (75ms)
      // Grants Ground Station (QGroundControl) clear airtime to send Arm, Takeoff, Mode, Waypoints, & Parameters!
      unsigned long rxStart = millis();
      unsigned long windowLimit = millis();
      while ((millis() - rxStart < 75) && (millis() - windowLimit < 160)) {
        // A. Concurrently read and parse F405 FC UART so ACKs are NEVER delayed!
        while (Serial.available()) {
          uint8_t b = Serial.read();
          fcByteCount++;
          parseAirMavlinkByte(b);
          if (stagingLen < sizeof(stagingBuf)) {
            stagingBuf[stagingLen++] = b;
          }
          lastStagingTime = millis();
        }

        // B. If F405 answered with COMMAND_ACK (Msg 77) or STATUSTEXT (Msg 253), flush IMMEDIATELY!
        if (fastFlushRequested && stagingLen > 0) {
          break; // Break out of receive window immediately to transmit ACK to Ground Station & QGC!
        }

        // C. Check for incoming uplink packets from Ground Station over LoRa
        if (loraReady) {
          int packetSize = LoRa.parsePacket();
          if (packetSize > 0) {
            addLog("LORA RX: %d B uplink -> sent to F405 (R6)", packetSize);
            while (LoRa.available()) {
              Serial.write((uint8_t)LoRa.read());
            }
            rxStart = millis(); // Reset timer to allow chained command packets
          } else if (readLoRaReg(0x18) & 0x08) {
            // Incoming packet is actively being received over the air -> extend window so it is not cut off!
            rxStart = millis();
          }
        }

        // D. Check for incoming MAVLink packets from QGC over Wi-Fi UDP (Port 14550)
        int udpRx;
        while ((udpRx = mavUdp.parsePacket()) > 0) {
          qgcClientIp = mavUdp.remoteIP();
          qgcClientPort = mavUdp.remotePort();
          qgcConnectedViaWifi = true;
          lastQgcUdpPacketTime = millis();
          addLog("WIFI RX: %d B from QGC -> sent to F405 (R6)", udpRx);
          while (mavUdp.available()) {
            Serial.write((uint8_t)mavUdp.read());
          }
        }

        vTaskDelay(2 / portTICK_PERIOD_MS);
      }
    } else {
      // 4. If idle, drain FC Serial and check for incoming LoRa & UDP packets
      while (Serial.available()) {
        uint8_t b = Serial.read();
        fcByteCount++;
        parseAirMavlinkByte(b);
        if (stagingLen < sizeof(stagingBuf)) {
          stagingBuf[stagingLen++] = b;
        }
        lastStagingTime = millis();
      }
      if (loraReady) {
        int packetSize = LoRa.parsePacket();
        if (packetSize > 0) {
          addLog("LORA RX: %d B uplink (idle) -> sent to F405 (R6)", packetSize);
          while (LoRa.available()) {
            Serial.write((uint8_t)LoRa.read());
          }
        }
      }
      int udpRx;
      while ((udpRx = mavUdp.parsePacket()) > 0) {
        qgcClientIp = mavUdp.remoteIP();
        qgcClientPort = mavUdp.remotePort();
        qgcConnectedViaWifi = true;
        lastQgcUdpPacketTime = millis();
        addLog("WIFI RX: %d B (idle) -> sent to F405 (R6)", udpRx);
        while (mavUdp.available()) {
          Serial.write((uint8_t)mavUdp.read());
        }
      }
    }

    // 5. Periodic Diagnostic Heartbeat (Every 1.2s) when idle
    if (millis() - lastAirDiagTime >= 1200) {
      lastAirDiagTime = millis();
      if (loraReady && stagingLen == 0) {
        AirDiagPacket diag;
        diag.header[0] = 0x24; // '$'
        diag.header[1] = 0x41; // 'A'
        diag.header[2] = 0x47; // 'G'
        diag.fcByteCount = fcByteCount;
        diag.loraPktCount = loraPktCount;
        diag.flags = 1;
        diag.checksum = calcChecksum((const uint8_t*)&diag, sizeof(diag) - 1);

        if (safeLoRaSend((const uint8_t*)&diag, sizeof(diag))) {
          loraPktCount++;
        }
      }
    }
    
    // Yield to FreeRTOS scheduler to prevent watchdog resets
    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// =================== TACTICAL FPV COCKPIT UI ===================
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
  <title>Personal UFO | FPV Cockpit</title>
  <style>
    :root {
      --bg: #070a12;
      --card-bg: rgba(15, 23, 42, 0.75);
      --card-border: rgba(30, 41, 59, 0.8);
      --accent: #00f0ff;
      --accent-glow: rgba(0, 240, 255, 0.35);
      --accent-subtle: rgba(0, 240, 255, 0.12);
      --amber: #ffb703;
      --amber-glow: rgba(255, 183, 3, 0.3);
      --red: #ff3366;
      --green: #00ff9d;
      --green-glow: rgba(0, 255, 157, 0.3);
      --text: #f1f5f9;
      --text-muted: #8492a6;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "SF Pro Display", monospace; -webkit-tap-highlight-color: transparent; }
    body { background: var(--bg); color: var(--text); padding: 12px; min-height: 100vh; overflow-x: hidden; }

    /* Top Bar */
    .top-bar { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; margin-bottom: 12px; gap: 8px; border-bottom: 1px solid var(--card-border); padding-bottom: 10px; }
    .brand { display: flex; align-items: center; gap: 8px; }
    .brand-icon { width: 10px; height: 10px; background: var(--accent); border-radius: 2px; box-shadow: 0 0 10px var(--accent); }
    .title { font-size: 14px; font-weight: 800; letter-spacing: 1.5px; color: var(--text); text-transform: uppercase; }
    .title span { color: var(--accent); }

    .telemetry-badges { display: flex; gap: 6px; flex-wrap: wrap; }
    .badge { font-size: 10px; font-weight: 700; padding: 4px 8px; border-radius: 4px; background: rgba(30, 41, 59, 0.6); border: 1px solid var(--card-border); color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; display: flex; align-items: center; gap: 5px; }
    .badge.online { background: rgba(0, 255, 157, 0.1); border-color: rgba(0, 255, 157, 0.3); color: var(--green); }
    .badge.radio { background: rgba(0, 240, 255, 0.1); border-color: rgba(0, 240, 255, 0.3); color: var(--accent); }
    .pulse-dot { width: 6px; height: 6px; border-radius: 50%; background: var(--green); box-shadow: 0 0 8px var(--green); animation: pulse 1.2s infinite; }
    @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.25; } 100% { opacity: 1; } }

    /* Grid Layout */
    .layout-grid { display: grid; grid-template-columns: 1fr; gap: 14px; max-width: 1300px; margin: 0 auto; }
    @media(min-width: 900px) {
      .layout-grid { grid-template-columns: 1.8fr 1.2fr; align-items: start; }
    }

    /* Video Viewport */
    .viewport-card { background: #000; border: 1px solid var(--card-border); border-radius: 10px; overflow: hidden; position: relative; box-shadow: 0 8px 30px rgba(0,0,0,0.8); }
    .stream-frame { position: relative; width: 100%; min-height: 280px; display: flex; justify-content: center; align-items: center; background: #020408; overflow: hidden; transition: min-height 0.3s ease; }
    .stream-frame img { width: 100%; height: auto; display: block; object-fit: contain; transform-origin: center center; transition: transform 0.25s cubic-bezier(0.16, 1, 0.3, 1); }
    .stream-standby { position: absolute; inset: 0; display: flex; flex-direction: column; justify-content: center; align-items: center; background: radial-gradient(circle at center, rgba(15, 23, 42, 0.95), #020408); z-index: 5; text-align: center; padding: 20px; }
    .stream-standby.hidden { display: none; }
    .standby-icon { width: 44px; height: 44px; border-radius: 50%; border: 2px solid var(--accent); display: flex; align-items: center; justify-content: center; margin-bottom: 12px; box-shadow: 0 0 15px var(--accent-subtle); color: var(--accent); font-weight: 900; font-size: 13px; letter-spacing: 1px; }
    .standby-title { font-size: 13px; font-weight: 800; color: #fff; letter-spacing: 1px; text-transform: uppercase; margin-bottom: 4px; }
    .standby-sub { font-size: 11px; color: var(--text-muted); margin-bottom: 14px; max-width: 260px; line-height: 1.4; }
    .btn-start-stream { background: var(--accent); color: #000; font-weight: 800; font-size: 11px; text-transform: uppercase; padding: 8px 16px; border-radius: 6px; border: none; cursor: pointer; box-shadow: 0 0 15px var(--accent-glow); transition: all 0.15s ease; }
    .btn-start-stream:hover { transform: scale(1.04); box-shadow: 0 0 20px var(--accent-glow); }

    /* Tactical HUD Overlays */
    .hud-layer { position: absolute; inset: 0; pointer-events: none; z-index: 10; padding: 10px; display: flex; flex-direction: column; justify-content: space-between; }
    .hud-top { display: flex; justify-content: space-between; align-items: center; }
    .hud-tag { background: rgba(2, 6, 23, 0.8); backdrop-filter: blur(6px); border: 1px solid rgba(255,255,255,0.12); padding: 4px 8px; border-radius: 4px; font-size: 10px; font-weight: 700; color: #fff; letter-spacing: 0.5px; display: flex; align-items: center; gap: 6px; }
    .rec-flag { color: var(--red); display: flex; align-items: center; gap: 4px; }
    .rec-flag::before { content: ""; display: inline-block; width: 6px; height: 6px; background: var(--red); border-radius: 50%; animation: pulse 1s infinite; }

    /* Crosshairs */
    .reticle { position: absolute; inset: 0; display: none; pointer-events: none; }
    .reticle.visible { display: block; }
    .reticle-center { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); width: 44px; height: 44px; border: 1px solid rgba(0, 240, 255, 0.4); border-radius: 50%; }
    .reticle-center::before, .reticle-center::after { content: ""; position: absolute; background: var(--accent); }
    .reticle-center::before { top: 50%; left: -8px; right: -8px; height: 1px; transform: translateY(-50%); }
    .reticle-center::after { left: 50%; top: -8px; bottom: -8px; width: 1px; transform: translateX(-50%); }

    /* HUD Bottom Controls */
    .hud-bottom-bar { display: flex; justify-content: space-between; align-items: center; pointer-events: auto; background: rgba(10, 15, 29, 0.85); backdrop-filter: blur(8px); padding: 8px 12px; border-top: 1px solid var(--card-border); gap: 6px; flex-wrap: wrap; }
    .action-btn { background: rgba(30, 41, 59, 0.8); border: 1px solid var(--card-border); color: #fff; padding: 6px 12px; border-radius: 6px; font-size: 11px; font-weight: 700; text-transform: uppercase; cursor: pointer; display: flex; align-items: center; gap: 6px; transition: all 0.15s ease; }
    .action-btn:hover { border-color: var(--accent); color: var(--accent); box-shadow: 0 0 10px var(--accent-subtle); }
    .action-btn.active { background: var(--accent); color: #000; border-color: var(--accent); box-shadow: 0 0 12px var(--accent-glow); }

    /* Control Deck Panels */
    .control-deck { display: flex; flex-direction: column; gap: 12px; }
    .panel { background: var(--card-bg); backdrop-filter: blur(12px); border: 1px solid var(--card-border); border-radius: 10px; padding: 14px; box-shadow: 0 4px 20px rgba(0,0,0,0.4); }
    .panel-header { font-size: 11px; font-weight: 800; color: var(--accent); text-transform: uppercase; letter-spacing: 1px; margin-bottom: 12px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid rgba(255,255,255,0.06); padding-bottom: 6px; }

    /* Flashlight Panel */
    .flash-quick-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-bottom: 10px; }
    .flash-pill { background: rgba(30, 41, 59, 0.6); border: 1px solid var(--card-border); color: var(--text-muted); padding: 6px 4px; border-radius: 6px; font-size: 10px; font-weight: 700; text-align: center; cursor: pointer; text-transform: uppercase; transition: all 0.15s; }
    .flash-pill:hover, .flash-pill.active { background: var(--amber-glow); border-color: var(--amber); color: var(--amber); }

    /* Rows and Sliders */
    .control-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; font-size: 12px; }
    .control-label { color: var(--text-muted); font-weight: 600; text-transform: uppercase; font-size: 11px; }
    .control-val { font-weight: 800; color: var(--accent); font-family: monospace; }

    input[type=range] { -webkit-appearance: none; width: 100%; background: #1e293b; height: 6px; border-radius: 3px; outline: none; margin: 4px 0 12px 0; }
    input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 18px; height: 18px; border-radius: 50%; background: var(--accent); cursor: pointer; border: 2px solid #000; box-shadow: 0 0 8px var(--accent); transition: transform 0.1s; }
    input[type=range]::-webkit-slider-thumb:hover { transform: scale(1.15); }

    /* Resolution Selector Buttons */
    .res-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-bottom: 12px; }
    .res-btn { background: rgba(30, 41, 59, 0.6); border: 1px solid var(--card-border); color: var(--text-muted); padding: 7px 4px; border-radius: 6px; font-size: 10px; font-weight: 700; text-align: center; cursor: pointer; transition: all 0.15s; }
    .res-btn:hover { border-color: var(--accent); color: #fff; }
    .res-btn.active { background: var(--accent); color: #000; border-color: var(--accent); box-shadow: 0 0 12px var(--accent-glow); }

    /* Rotation & Invert Controls */
    .rot-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-bottom: 10px; }
    .rot-btn { background: rgba(30, 41, 59, 0.6); border: 1px solid var(--card-border); color: var(--text-muted); padding: 7px 4px; border-radius: 6px; font-size: 10px; font-weight: 700; text-align: center; cursor: pointer; transition: all 0.15s; text-transform: uppercase; }
    .rot-btn:hover { border-color: var(--accent); color: #fff; }
    .rot-btn.active { background: var(--accent); color: #000; border-color: var(--accent); box-shadow: 0 0 12px var(--accent-glow); }

    .toggle-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
    .toggle-card { background: rgba(30, 41, 59, 0.4); border: 1px solid var(--card-border); padding: 8px 10px; border-radius: 6px; display: flex; justify-content: space-between; align-items: center; cursor: pointer; transition: border-color 0.15s; }
    .toggle-card:hover { border-color: var(--accent); }
    .toggle-card.active { border-color: var(--accent); background: var(--accent-subtle); }
    .toggle-title { font-size: 11px; font-weight: 700; text-transform: uppercase; color: var(--text); }

    /* Dropdowns */
    select.select-field { width: 100%; background: rgba(30, 41, 59, 0.8); border: 1px solid var(--card-border); color: #fff; padding: 7px 10px; border-radius: 6px; font-size: 11px; font-weight: 700; outline: none; margin-bottom: 10px; }

    /* Toast Notification */
    .toast-container { position: fixed; bottom: 16px; left: 16px; z-index: 100; pointer-events: none; }
    .toast { background: rgba(15, 23, 42, 0.95); border: 1px solid var(--accent); color: var(--accent); padding: 6px 14px; border-radius: 6px; font-size: 11px; font-weight: 800; letter-spacing: 0.5px; text-transform: uppercase; box-shadow: 0 4px 15px rgba(0,0,0,0.6); opacity: 0; transform: translateY(10px); transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1); }
    .toast.show { opacity: 1; transform: translateY(0); }
  </style>
</head>
<body>
  <div class="top-bar">
    <div class="brand">
      <div class="brand-icon"></div>
      <div class="title">PERSONAL UFO <span>FPV COCKPIT</span></div>
    </div>
    <div class="telemetry-badges">
      <div class="badge online"><span class="pulse-dot"></span>Stream :81</div>
      <div class="badge" id="hudResBadge">QVGA 30FPS</div>
      <div class="badge radio" id="loraBadge">LoRa 433 MHz</div>
      <div class="badge online" id="udpBadge">MAV UDP :14550</div>
      <a href="/update" class="badge radio" style="text-decoration:none;cursor:pointer;">OTA Flash</a>
      <a href="/logs" target="_blank" class="badge" style="text-decoration:none;cursor:pointer;color:var(--amber);border-color:var(--amber);">Live Logs</a>
      <div class="badge" style="color:var(--accent);">FW v2.3</div>
      <div class="badge" id="fcBadge">FC: 0 B</div>
      <div class="badge" id="modeBadge">STABILIZE</div>
      <div class="badge" id="batBadge">--.-V (--%)</div>
      <div class="badge" id="altBadge">ALT: 0.0m</div>
      <div class="badge" id="satBadge">SAT: 0</div>
    </div>
  </div>

  <div class="layout-grid">
    <!-- Viewport Container -->
    <div class="viewport-card">
      <div class="stream-frame" id="streamFrame">
        <div class="hud-layer">
          <div class="hud-top">
            <div class="hud-tag" id="hudModeTag">[DISARMED] STABILIZE</div>
            <div class="hud-tag" id="hudTimerTag">00:00 | SAT: 0</div>
          </div>
          <div style="display:flex; justify-content:space-between; align-items:flex-end;">
            <div class="hud-tag" id="hudBatTag">BAT: --.-V (--%)</div>
            <div class="hud-tag" id="hudAltTag">ALT: 0.0m | 0kph</div>
          </div>
        </div>
        <div class="reticle" id="hudReticle">
          <div class="reticle-center"></div>
        </div>
        <div id="streamStandby" class="stream-standby">
          <div class="standby-icon">CAM</div>
          <div class="standby-title">Video Stream Standby</div>
          <div class="standby-sub">Video streaming is paused to save drone battery, Wi-Fi bandwidth, and keep camera cool.</div>
          <button class="btn-start-stream" onclick="toggleStream(true)">Start Live Video</button>
        </div>
        <img id="streamImg" crossorigin="anonymous" alt="Stream Standby">
      </div>

      <div class="hud-bottom-bar">
        <div style="display: flex; gap: 6px; flex-wrap: wrap;">
          <button class="action-btn" id="btnStreamToggle" onclick="toggleStream()">Start Stream</button>
          <button class="action-btn" id="btnSnapPhoto" onclick="takeInstantPhoto()" style="color:var(--accent);">Snap Photo</button>
          <button class="action-btn" id="btnRecordToggle" onclick="toggleRecord()" style="color:var(--red);">Record</button>
          <button class="action-btn" id="btnQuickRotate" onclick="cycleQuickRotate()">Rot +90°</button>
          <button class="action-btn" id="btnReticle" onclick="toggleReticle()">Reticle</button>
        </div>
        <button class="action-btn" onclick="toggleFullscreen()">Expand</button>
      </div>
    </div>

    <!-- Live Controls Deck -->
    <div class="control-deck">
      <!-- Searchlight / Flash LED (GPIO 4) -->
      <div class="panel">
        <div class="panel-header">
          <span>Searchlight / Flash LED (GPIO 4)</span>
          <span id="flashStateLabel" style="color:var(--amber); font-family:monospace;">OFF</span>
        </div>
        <div class="flash-quick-grid">
          <button class="flash-pill active" id="btnFlashOff" onclick="setFlashPreset(0)">OFF</button>
          <button class="flash-pill" id="btnFlash25" onclick="setFlashPreset(64)">25%</button>
          <button class="flash-pill" id="btnFlash50" onclick="setFlashPreset(128)">50%</button>
          <button class="flash-pill" id="btnFlash100" onclick="setFlashPreset(255)">MAX</button>
        </div>
        <div class="control-row">
          <span class="control-label">Continuous Dimmer</span>
          <span class="control-val" id="flashVal">0%</span>
        </div>
        <input type="range" id="flashSlider" min="0" max="255" value="0" oninput="onFlashSliderInput(this.value)">
      </div>

      <!-- Mount Orientation & Live 90° Rotation -->
      <div class="panel">
        <div class="panel-header">
          <span>Camera Tilt & Mount Orientation</span>
          <span id="rotDegLabel" style="color:var(--accent); font-family:monospace;">0°</span>
        </div>
        
        <!-- Live GPU-Accelerated 90° Rotation Buttons -->
        <div class="control-row">
          <span class="control-label">Live View Rotation</span>
        </div>
        <div class="rot-grid">
          <button class="rot-btn active" id="rot0" onclick="setLiveRotation(0)">0° Normal</button>
          <button class="rot-btn" id="rot90" onclick="setLiveRotation(90)">90° CW</button>
          <button class="rot-btn" id="rot180" onclick="setLiveRotation(180)">180° Invert</button>
          <button class="rot-btn" id="rot270" onclick="setLiveRotation(270)">270° CW</button>
        </div>

        <!-- Hardware Sensor Inversions -->
        <div class="toggle-grid" style="margin-top:6px;">
          <div class="toggle-card" id="cardVflip" onclick="toggleInvert('vflip')">
            <span class="toggle-title">V-Flip (Inverted)</span>
          </div>
          <div class="toggle-card" id="cardHmirror" onclick="toggleInvert('hmirror')">
            <span class="toggle-title">H-Mirror</span>
          </div>
        </div>
      </div>

      <!-- Resolution & Compression -->
      <div class="panel">
        <div class="panel-header">
          <span>Optical Resolution (Port 81)</span>
          <span style="color:var(--text-muted); font-size:9px;">INSTANT REBIND</span>
        </div>
        <div class="res-grid">
          <button class="res-btn active" id="res5" onclick="setResolution(5, 'QVGA (320x240)')">QVGA 30FPS</button>
          <button class="res-btn" id="res6" onclick="setResolution(6, 'CIF (400x296)')">CIF 25FPS</button>
          <button class="res-btn" id="res8" onclick="setResolution(8, 'VGA (640x480)')">VGA 20FPS</button>
          <button class="res-btn" id="res9" onclick="setResolution(9, 'SVGA (800x600)')">SVGA</button>
          <button class="res-btn" id="res11" onclick="setResolution(11, 'HD (1280x720)')">HD 720P</button>
          <button class="res-btn" id="res13" onclick="setResolution(13, 'UXGA (1600x1200)')">2MP STILL</button>
        </div>

        <div class="control-row">
          <span class="control-label">JPEG Compression</span>
          <span class="control-val" id="qualityVal">12</span>
        </div>
        <input type="range" min="10" max="40" value="12" id="qualitySlider" oninput="sendLiveControl('quality', this.value, 'qualityVal')">
      </div>

      <!-- Sensor Image Adjustments -->
      <div class="panel">
        <div class="panel-header">Sensor Image Tuning</div>
        <div class="control-row">
          <span class="control-label">Brightness</span>
          <span class="control-val" id="brightVal">0</span>
        </div>
        <input type="range" min="-2" max="2" value="0" oninput="sendLiveControl('brightness', this.value, 'brightVal')">

        <div class="control-row">
          <span class="control-label">Contrast</span>
          <span class="control-val" id="contrastVal">0</span>
        </div>
        <input type="range" min="-2" max="2" value="0" oninput="sendLiveControl('contrast', this.value, 'contrastVal')">

        <div class="control-row">
          <span class="control-label">Special Effect</span>
        </div>
        <select class="select-field" onchange="sendLiveControl('special_effect', this.value)">
          <option value="0" selected>Normal</option>
          <option value="1">Negative</option>
          <option value="2">Grayscale</option>
          <option value="3">Red Tint</option>
          <option value="4">Green Tint</option>
          <option value="5">Blue Tint</option>
          <option value="6">Sepia</option>
        </select>
      </div>
    </div>
  </div>

  <div class="toast-container">
    <div class="toast" id="toastBox">Ready</div>
  </div>

  <script>
    // Direct stream connection to dedicated Port 81
    const streamPort = 81;
    const streamUrl = window.location.protocol + '//' + window.location.hostname + ':' + streamPort + '/stream';
    const streamImg = document.getElementById('streamImg');
    const streamFrame = document.getElementById('streamFrame');
    const streamStandby = document.getElementById('streamStandby');
    const btnStreamToggle = document.getElementById('btnStreamToggle');
    const streamTag = document.getElementById('streamStatusTag');
    let isStreaming = false;

    // On-demand stream controller
    function toggleStream(forceStart) {
      if (forceStart === true || !isStreaming) {
        isStreaming = true;
        streamImg.src = streamUrl + '?t=' + Date.now();
        streamStandby.classList.add('hidden');
        btnStreamToggle.innerText = 'Pause Stream';
        btnStreamToggle.classList.add('active');
        if (streamTag) {
          streamTag.innerText = 'LIVE STREAM';
          streamTag.classList.add('rec-flag');
        }
        showToast('STREAM STARTED (:81)');
      } else {
        isStreaming = false;
        streamImg.src = '';
        streamStandby.classList.remove('hidden');
        btnStreamToggle.innerText = 'Start Stream';
        btnStreamToggle.classList.remove('active');
        if (streamTag) {
          streamTag.innerText = 'STANDBY';
          streamTag.classList.remove('rec-flag');
        }
        showToast('STREAM PAUSED (IDLE)');
      }
    }

    let vflipState = 0;
    let hmirrorState = 0;
    let reticleState = false;
    let toastTimer = null;
    let currentRotation = 0;

    // Toast Alert Helper
    function showToast(msg) {
      const box = document.getElementById('toastBox');
      box.innerText = msg;
      box.classList.add('show');
      clearTimeout(toastTimer);
      toastTimer = setTimeout(() => box.classList.remove('show'), 1400);
    }

    // High-performance Non-blocking Control Dispatcher
    function sendLiveControl(varName, value, labelId) {
      if (labelId) document.getElementById(labelId).innerText = value;
      
      // Send to Port 80 dedicated controls endpoint
      fetch(`/control?var=${varName}&val=${value}`)
        .then(() => showToast(`${varName.toUpperCase()}: ${value}`))
        .catch(err => console.error("Control dispatch failed", err));
    }

    // Live GPU-Accelerated 90-Degree Rotation Engine
    function setLiveRotation(deg) {
      currentRotation = parseInt(deg);
      
      // Highlight active button
      document.querySelectorAll('.rot-btn').forEach(btn => btn.classList.remove('active'));
      const activeBtn = document.getElementById('rot' + currentRotation);
      if (activeBtn) activeBtn.classList.add('active');

      document.getElementById('rotDegLabel').innerText = currentRotation + '°';

      // Apply GPU transform
      if (currentRotation === 90 || currentRotation === 270) {
        streamImg.style.transform = `rotate(${currentRotation}deg) scale(0.75)`;
        streamFrame.style.minHeight = '370px'; // Give portrait headroom
      } else if (currentRotation === 180) {
        streamImg.style.transform = `rotate(180deg) scale(1)`;
        streamFrame.style.minHeight = '280px';
      } else {
        streamImg.style.transform = `rotate(0deg) scale(1)`;
        streamFrame.style.minHeight = '280px';
      }

      // Persist to browser localStorage so refresh remembers tilt
      localStorage.setItem('ufo_cam_rot', currentRotation);
      showToast(`ROTATED: ${currentRotation}°`);
    }

    function cycleQuickRotate() {
      const nextRot = (currentRotation + 90) % 360;
      setLiveRotation(nextRot);
    }

    // Load saved rotation on startup
    const savedRot = localStorage.getItem('ufo_cam_rot');
    if (savedRot !== null) {
      setLiveRotation(parseInt(savedRot));
    }

    // Flash LED Controls
    function setFlashPreset(duty) {
      document.getElementById('flashSlider').value = duty;
      onFlashSliderInput(duty);
    }

    function onFlashSliderInput(duty) {
      const percent = Math.round((duty / 255) * 100);
      document.getElementById('flashVal').innerText = percent + '%';
      
      const label = document.getElementById('flashStateLabel');
      label.innerText = duty > 0 ? `${percent}% ON` : 'OFF';
      label.style.color = duty > 0 ? 'var(--green)' : 'var(--amber)';

      // Highlight preset buttons
      document.getElementById('btnFlashOff').classList.toggle('active', duty == 0);
      document.getElementById('btnFlash25').classList.toggle('active', duty > 0 && duty <= 64);
      document.getElementById('btnFlash50').classList.toggle('active', duty > 64 && duty <= 150);
      document.getElementById('btnFlash100').classList.toggle('active', duty > 150);

      sendLiveControl('flash', duty);
    }

    // Resolution Switcher with Safe DMA Sensor Rebind
    function setResolution(resVal, resLabel) {
      document.querySelectorAll('.res-btn').forEach(btn => btn.classList.remove('active'));
      const activeBtn = document.getElementById('res' + resVal);
      if (activeBtn) activeBtn.classList.add('active');

      const hudBadge = document.getElementById('hudResBadge');
      if (hudBadge) hudBadge.innerText = resLabel;
      showToast(`SETTING ${resLabel}`);

      const wasStreaming = isStreaming;
      if (wasStreaming) {
        streamImg.src = '';
      }

      fetch(`/control?var=framesize&val=${resVal}`)
        .then(r => r.text())
        .then(() => {
          showToast(`RES: ${resLabel}`);
          if (wasStreaming) {
            setTimeout(() => {
              streamImg.crossOrigin = "anonymous";
              streamImg.src = streamUrl + '?t=' + Date.now();
            }, 300);
          }
        })
        .catch(err => {
          console.error(err);
          showToast('RES SWITCH FAILED');
          if (wasStreaming) {
            streamImg.crossOrigin = "anonymous";
            streamImg.src = streamUrl + '?t=' + Date.now();
          }
        });
    }

    // Hardware Invert Toggles
    function toggleInvert(type) {
      if (type === 'vflip') {
        vflipState = vflipState ? 0 : 1;
        document.getElementById('cardVflip').classList.toggle('active', vflipState);
        sendLiveControl('vflip', vflipState);
      } else if (type === 'hmirror') {
        hmirrorState = hmirrorState ? 0 : 1;
        document.getElementById('cardHmirror').classList.toggle('active', hmirrorState);
        sendLiveControl('hmirror', hmirrorState);
      }
    }

    // Reticle HUD Crosshair
    function toggleReticle() {
      reticleState = !reticleState;
      document.getElementById('hudReticle').classList.toggle('visible', reticleState);
      document.getElementById('btnReticle').classList.toggle('active', reticleState);
      showToast(reticleState ? 'RETICLE ACTIVE' : 'RETICLE HIDDEN');
    }

    // Stream Reconnection Helper
    function reconnectStream() {
      if (isStreaming) {
        streamImg.crossOrigin = "anonymous";
        streamImg.src = streamUrl + '?t=' + Date.now();
        showToast('STREAM RECONNECTED');
      } else {
        toggleStream(true);
      }
    }

    // Instant High-Resolution Photo Capture (No Live Streaming Needed!)
    function takeInstantPhoto() {
      const snapBtn = document.getElementById('btnSnapPhoto');
      if (snapBtn) snapBtn.innerText = 'Snapping...';
      showToast('CAPTURING PHOTO...');

      // Shutter flash effect
      const frame = document.getElementById('streamFrame');
      frame.style.filter = 'brightness(2.2)';
      setTimeout(() => frame.style.filter = 'brightness(1)', 120);

      fetch('/capture?t=' + Date.now())
        .then(response => {
          if (!response.ok) throw new Error('Capture failed');
          return response.blob();
        })
        .then(blob => {
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          const now = new Date();
          const pad = n => (n < 10 ? '0' : '') + n;
          const stamp = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + '_' +
                        pad(now.getHours()) + '-' + pad(now.getMinutes()) + '-' + pad(now.getSeconds());
          a.href = url;
          a.download = 'ufo_photo_' + stamp + '.jpg';
          document.body.appendChild(a);
          a.click();
          setTimeout(() => {
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
          }, 1200);

          // If stream is NOT running, show preview for 3.5 seconds
          if (!isStreaming) {
            streamImg.src = url;
            streamStandby.classList.add('hidden');
            setTimeout(() => {
              if (!isStreaming) {
                streamStandby.classList.remove('hidden');
                streamImg.src = '';
              }
            }, 3500);
          }

          if (snapBtn) snapBtn.innerText = 'Snap Photo';
          const kb = Math.round(blob.size / 1024);
          showToast('PHOTO SAVED (' + kb + ' KB)');
        })
        .catch(err => {
          console.error(err);
          if (snapBtn) snapBtn.innerText = 'Snap Photo';
          showToast('CAPTURE ERROR');
        });
    }

    // Native Fullscreen Toggle
    function toggleFullscreen() {
      const frame = document.getElementById('streamFrame');
      if (!document.fullscreenElement) {
        frame.requestFullscreen().catch(err => alert(err.message));
      } else {
        document.exitFullscreen();
      }
    }

    // Dynamic telemetry status polling & FPV OSD Engine
    let latestTelem = { mode: 'DISARMED', armed: false, batV: 0, batPct: 0, alt: 0, sats: 0, spd: 0, telem: false };
    let bootTime = Date.now();
    let mediaRecorder = null;
    let recordedChunks = [];
    let recordCanvas = document.createElement('canvas');
    let recordCtx = recordCanvas.getContext('2d');
    let recordInterval = null;
    let recordStartTime = 0;
    let isRecording = false;
    let selectedMime = '';

    function formatTimer(ms) {
      const totalSec = Math.floor((ms || (Date.now() - bootTime)) / 1000);
      const m = Math.floor(totalSec / 60);
      const s = totalSec % 60;
      return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
    }

    // In-Browser DVR Recording Engine with Burned-In OSD Telemetry
    function toggleRecord() {
      if (!isRecording) {
        startRecording();
      } else {
        stopRecording();
      }
    }

    function startRecording() {
      if (!isStreaming) {
        showToast('START STREAM FIRST TO RECORD');
        return;
      }
      recordedChunks = [];
      const w = streamImg.naturalWidth || 640;
      const h = streamImg.naturalHeight || 480;
      recordCanvas.width = w;
      recordCanvas.height = h;

      // Prime canvas to solid background
      recordCtx.fillStyle = '#020408';
      recordCtx.fillRect(0, 0, w, h);

      const canvasStream = recordCanvas.captureStream(25);
      
      const candidateMimes = [
        'video/webm;codecs=vp9,opus',
        'video/webm;codecs=vp8,opus',
        'video/webm;codecs=vp9',
        'video/webm;codecs=vp8',
        'video/webm',
        'video/mp4;codecs=avc1',
        'video/mp4'
      ];
      selectedMime = '';
      for (let m of candidateMimes) {
        if (typeof MediaRecorder !== 'undefined' && MediaRecorder.isTypeSupported(m)) {
          selectedMime = m;
          break;
        }
      }

      try {
        const options = selectedMime ? { mimeType: selectedMime } : {};
        mediaRecorder = new MediaRecorder(canvasStream, options);
      } catch (e) {
        console.error('MediaRecorder error', e);
        showToast('RECORDER NOT SUPPORTED');
        return;
      }

      mediaRecorder.ondataavailable = e => {
        if (e.data && e.data.size > 0) recordedChunks.push(e.data);
      };

      mediaRecorder.onstop = () => {
        if (recordedChunks.length === 0) {
          showToast('RECORDING EMPTY');
          return;
        }
        const isMp4 = (selectedMime && selectedMime.includes('mp4'));
        const ext = isMp4 ? 'mp4' : 'webm';
        const blob = new Blob(recordedChunks, { type: selectedMime || 'video/webm' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        const now = new Date();
        const pad = n => (n < 10 ? '0' : '') + n;
        const stamp = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + '_' +
                      pad(now.getHours()) + '-' + pad(now.getMinutes()) + '-' + pad(now.getSeconds());
        a.href = url;
        a.download = 'ufo_flight_' + stamp + '.' + ext;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => {
          document.body.removeChild(a);
          URL.revokeObjectURL(url);
        }, 1200);
        const kb = Math.round(blob.size / 1024);
        showToast('SAVED ' + kb + ' KB (.' + ext.toUpperCase() + ')');
      };

      // 250ms timeslice ensures chunks flush continuously to disk
      mediaRecorder.start(250);
      isRecording = true;
      recordStartTime = Date.now();

      // DVR Render Loop: Composites live video + burned-in FPV OSD Telemetry
      recordInterval = setInterval(() => {
        if (!isRecording) return;
        
        const cw = recordCanvas.width;
        const ch = recordCanvas.height;

        try {
          if (streamImg.complete && streamImg.naturalWidth > 0) {
            if (recordCanvas.width !== streamImg.naturalWidth || recordCanvas.height !== streamImg.naturalHeight) {
              recordCanvas.width = streamImg.naturalWidth;
              recordCanvas.height = streamImg.naturalHeight;
            }
            recordCtx.drawImage(streamImg, 0, 0, recordCanvas.width, recordCanvas.height);
          } else {
            recordCtx.fillStyle = '#050810';
            recordCtx.fillRect(0, 0, cw, ch);
          }
        } catch (err) {
          recordCtx.fillStyle = '#050810';
          recordCtx.fillRect(0, 0, cw, ch);
        }

        // Burn-in OSD Telemetry
        recordCtx.font = 'bold 12px monospace';
        recordCtx.fillStyle = 'rgba(2, 6, 23, 0.75)';
        recordCtx.fillRect(6, 6, 136, 22);
        recordCtx.fillRect(cw - 132, 6, 126, 22);
        recordCtx.fillRect(6, ch - 28, 146, 22);
        recordCtx.fillRect(cw - 138, ch - 28, 132, 22);

        recordCtx.fillStyle = latestTelem.armed ? '#00ff9d' : '#f59e0b';
        recordCtx.fillText((latestTelem.armed ? 'ARM ' : 'DISARM ') + (latestTelem.mode || 'STAB'), 10, 21);

        recordCtx.fillStyle = '#ffffff';
        recordCtx.fillText('SAT:' + (latestTelem.sats || 0) + ' ' + formatTimer(), cw - 124, 21);

        recordCtx.fillStyle = '#00f0ff';
        recordCtx.fillText('BAT:' + (latestTelem.batV ? latestTelem.batV.toFixed(1) + 'V' : '--V') + ' (' + (latestTelem.batPct || 0) + '%)', 10, ch - 13);

        recordCtx.fillText('ALT:' + (latestTelem.alt ? latestTelem.alt.toFixed(1) + 'm' : '0m') + ' ' + (latestTelem.spd ? latestTelem.spd.toFixed(0) : '0') + 'kph', cw - 132, ch - 13);
      }, 40);

      const recBtn = document.getElementById('btnRecordToggle');
      recBtn.innerText = 'REC [00:00]';
      recBtn.classList.add('active');
      showToast('RECORDING STARTED');
    }

    function stopRecording() {
      if (!isRecording) return;
      isRecording = false;
      clearInterval(recordInterval);
      if (mediaRecorder && mediaRecorder.state !== 'inactive') {
        mediaRecorder.stop();
      }
      const recBtn = document.getElementById('btnRecordToggle');
      recBtn.innerText = 'Record';
      recBtn.classList.remove('active');
    }

    function pollStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          latestTelem = data;
          const badge = document.getElementById('loraBadge');
          if (badge) {
            badge.innerText = data.lora ? 'LoRa [Armed]' : 'LoRa [Standby]';
            badge.className = data.lora ? 'badge online' : 'badge radio';
          }
          const fcBadge = document.getElementById('fcBadge');
          if (fcBadge) {
            if (data.fcBytes > 1024) {
              fcBadge.innerText = 'FC: ' + (data.fcBytes / 1024).toFixed(1) + ' KB';
              fcBadge.className = 'badge online';
            } else if (data.fcBytes > 0) {
              fcBadge.innerText = 'FC: ' + data.fcBytes + ' B';
              fcBadge.className = 'badge online';
            } else {
              fcBadge.innerText = 'FC: 0 B';
              fcBadge.className = 'badge';
            }
          }
          
          // Flight mode badge & OSD
          const modeBadge = document.getElementById('modeBadge');
          if (modeBadge) {
            modeBadge.innerText = (data.armed ? 'ARM: ' : '') + data.mode;
            modeBadge.className = data.telem ? 'badge online' : 'badge';
          }
          const hudModeTag = document.getElementById('hudModeTag');
          if (hudModeTag) {
            hudModeTag.innerText = (data.armed ? '[ARMED] ' : '[DISARMED] ') + data.mode;
            hudModeTag.style.color = data.armed ? 'var(--green)' : 'var(--amber)';
          }

          // Battery badge & OSD
          const batBadge = document.getElementById('batBadge');
          if (batBadge) {
            batBadge.innerText = data.batV > 5 ? (data.batV.toFixed(1) + 'V (' + data.batPct + '%)') : '--.-V (--%)';
            batBadge.className = (data.batPct > 20) ? 'badge online' : 'badge';
          }
          const hudBatTag = document.getElementById('hudBatTag');
          if (hudBatTag) {
            hudBatTag.innerText = 'BAT: ' + (data.batV > 5 ? (data.batV.toFixed(1) + 'V (' + data.batPct + '%)') : '--.-V');
          }

          // Altitude & Speed OSD
          const altBadge = document.getElementById('altBadge');
          if (altBadge) altBadge.innerText = 'ALT: ' + data.alt.toFixed(1) + 'm';
          const hudAltTag = document.getElementById('hudAltTag');
          if (hudAltTag) hudAltTag.innerText = 'ALT: ' + data.alt.toFixed(1) + 'm | ' + data.spd.toFixed(0) + 'kph';

          // Satellites
          const satBadge = document.getElementById('satBadge');
          if (satBadge) satBadge.innerText = 'SAT: ' + data.sats;
          const hudTimerTag = document.getElementById('hudTimerTag');
          if (hudTimerTag) hudTimerTag.innerText = formatTimer() + ' | SAT: ' + data.sats;

          // Recording timer ticker
          if (isRecording) {
            const recElapsed = Date.now() - recordStartTime;
            document.getElementById('btnRecordToggle').innerText = 'REC [' + formatTimer(recElapsed) + ']';
          }
        })
        .catch(() => {});
    }
    setInterval(pollStatus, 2000);
    pollStatus();
  </script>
</body>
</html>
)rawliteral";

// =================== HTTP URI HANDLERS ===================
// 1. Root handler (Port 80): Serves the Web Cockpit UI
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "identity");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// 2. High-speed Dedicated MJPEG Stream Handler (Port 81)
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    _jpg_buf_len = fb->len;
    _jpg_buf = fb->buf;

    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    esp_camera_fb_return(fb);
    fb = NULL;

    if (res != ESP_OK) break;
    vTaskDelay(20 / portTICK_PERIOD_MS); // ~25-35 FPS pacing
  }
  return res;
}

// 3. Single Frame Capture Handler (Port 80 - Instant Photo)
static esp_err_t capture_handler(httpd_req_t *req) {
  // Flush older buffered frame to guarantee instant current photo
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    return httpd_resp_send_500(req);
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=ufo_photo.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// 4. Real-Time Hardware & Sensor Control Handler (Port 80 - NEVER BLOCKS!)
static esp_err_t control_handler(httpd_req_t *req) {
  char buf[64];
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1 && buf_len <= sizeof(buf)) {
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      char var[16] = {0};
      char val_str[16] = {0};
      if (httpd_query_key_value(buf, "var", var, sizeof(var)) == ESP_OK &&
          httpd_query_key_value(buf, "val", val_str, sizeof(val_str)) == ESP_OK) {
        int val = atoi(val_str);
        sensor_t * s = esp_camera_sensor_get();
        int res = 0;

        if (!strcmp(var, "framesize")) {
          if (s && s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
            if (res == 0) currentFramesize = val;
          }
        } else if (!strcmp(var, "quality")) {
          if (s) {
            res = s->set_quality(s, val);
            if (res == 0) currentQuality = val;
          }
        } else if (!strcmp(var, "contrast")) {
          if (s) {
            res = s->set_contrast(s, val);
            if (res == 0) currentContrast = val;
          }
        } else if (!strcmp(var, "brightness")) {
          if (s) {
            res = s->set_brightness(s, val);
            if (res == 0) currentBrightness = val;
          }
        } else if (!strcmp(var, "saturation")) {
          if (s) {
            res = s->set_saturation(s, val);
            if (res == 0) currentSaturation = val;
          }
        } else if (!strcmp(var, "special_effect")) {
          if (s) {
            res = s->set_special_effect(s, val);
            if (res == 0) currentEffect = val;
          }
        } else if (!strcmp(var, "vflip")) {
          if (s) {
            res = s->set_vflip(s, val);
            if (res == 0) currentVflip = val;
          }
        } else if (!strcmp(var, "hmirror")) {
          if (s) {
            res = s->set_hmirror(s, val);
            if (res == 0) currentHmirror = val;
          }
        } else if (!strcmp(var, "flash")) {
          setFlashBrightness(val);
          res = 0;
        } else {
          res = -1;
        }

        if (res < 0) return httpd_resp_send_500(req);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "OK", 2);
      }
    }
  }
  return httpd_resp_send_404(req);
}

// 5. System Status Handler (Port 80)
static esp_err_t status_handler(httpd_req_t *req) {
  bool telemAlive = (millis() - lastMavlinkTime < 3500) && (lastMavlinkTime > 0);
  char json[260];
  snprintf(json, sizeof(json), 
    "{\"lora\":%s,\"fcBytes\":%u,\"pkts\":%u,\"flash\":%d,\"res\":%d,"
    "\"telem\":%s,\"mode\":\"%s\",\"armed\":%s,\"batV\":%.1f,\"batPct\":%d,"
    "\"alt\":%.1f,\"sats\":%d,\"spd\":%.1f}", 
    loraReady ? "true" : "false", fcByteCount, loraPktCount, currentFlashDuty, currentFramesize,
    telemAlive ? "true" : "false", flightMode, droneArmed ? "true" : "false",
    droneBatVolts, droneBatPct, droneAltitude, droneSats, droneSpeed);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

// =================== WIRELESS OTA FIRMWARE FLASH UI ===================
static const char PROGMEM UPDATE_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Personal UFO | Wireless OTA Firmware Flash</title>
  <style>
    :root {
      --bg: #070a12;
      --card-bg: rgba(15, 23, 42, 0.85);
      --card-border: rgba(30, 41, 59, 0.9);
      --accent: #00f0ff;
      --accent-glow: rgba(0, 240, 255, 0.35);
      --green: #00ff9d;
      --red: #ff3366;
      --text: #f1f5f9;
      --text-muted: #8492a6;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace; }
    body { background: var(--bg); color: var(--text); padding: 20px; min-height: 100vh; display: flex; align-items: center; justify-content: center; }
    .card { background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 12px; padding: 24px; max-width: 480px; width: 100%; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
    .header { display: flex; align-items: center; gap: 10px; margin-bottom: 16px; border-bottom: 1px solid var(--card-border); padding-bottom: 12px; }
    .icon { width: 12px; height: 12px; background: var(--accent); border-radius: 2px; box-shadow: 0 0 10px var(--accent); }
    h2 { font-size: 16px; font-weight: 800; letter-spacing: 1px; text-transform: uppercase; }
    h2 span { color: var(--accent); }
    p { font-size: 13px; color: var(--text-muted); line-height: 1.5; margin-bottom: 16px; }
    .drop-zone { border: 2px dashed var(--card-border); border-radius: 8px; padding: 24px 16px; text-align: center; margin-bottom: 16px; cursor: pointer; transition: 0.2s; }
    .drop-zone:hover { border-color: var(--accent); background: rgba(0, 240, 255, 0.05); }
    input[type="file"] { display: none; }
    .file-name { font-size: 12px; color: var(--accent); margin-top: 8px; font-weight: 600; word-break: break-all; }
    .btn { width: 100%; padding: 12px; border: none; border-radius: 6px; font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 1px; cursor: pointer; background: var(--accent); color: #000; box-shadow: 0 0 15px var(--accent-glow); transition: 0.2s; }
    .btn:hover { background: #38f8ff; box-shadow: 0 0 25px var(--accent); }
    .btn:disabled { background: #334155; color: #64748b; box-shadow: none; cursor: not-allowed; }
    .progress-wrap { display: none; margin-top: 16px; }
    .bar-bg { width: 100%; height: 8px; background: rgba(30, 41, 59, 0.8); border-radius: 4px; overflow: hidden; margin-bottom: 6px; }
    .bar-fill { width: 0%; height: 100%; background: var(--green); transition: width 0.15s linear; box-shadow: 0 0 10px var(--green); }
    .progress-meta { display: flex; justify-content: space-between; font-size: 11px; color: var(--text-muted); }
    .status { margin-top: 12px; font-size: 12px; font-weight: 600; text-align: center; }
    .back-link { display: block; text-align: center; margin-top: 16px; font-size: 12px; color: var(--text-muted); text-decoration: none; }
    .back-link:hover { color: var(--accent); }
  </style>
</head>
<body>
  <div class="card">
    <div class="header">
      <div class="icon"></div>
      <h2>Personal UFO | <span>OTA Flash</span></h2>
    </div>
    <p>Wirelessly update the Air Unit firmware over Wi-Fi. Export the <b>.bin</b> file in Arduino IDE (Ctrl+Alt+S) and upload here.</p>

    <div class="drop-zone" onclick="document.getElementById('binFile').click()">
      <div style="font-size: 24px; margin-bottom: 6px;">&#9881;</div>
      <div style="font-size: 13px; font-weight: 600;">Click to select firmware (.bin)</div>
      <div class="file-name" id="fileName">No file selected</div>
    </div>
    <input type="file" id="binFile" accept=".bin" onchange="onFileSelected(this)">

    <button id="uploadBtn" class="btn" disabled onclick="startOTA()">Flash Firmware</button>

    <div class="progress-wrap" id="progWrap">
      <div class="bar-bg"><div class="bar-fill" id="barFill"></div></div>
      <div class="progress-meta">
        <span id="progLabel">Flashing flash memory...</span>
        <span id="progPct">0%</span>
      </div>
    </div>

    <div class="status" id="statusText"></div>
    <a href="/" class="back-link">&larr; Return to Cockpit HUD</a>
  </div>

  <script>
    let selectedFile = null;
    function onFileSelected(input) {
      if (input.files && input.files[0]) {
        selectedFile = input.files[0];
        document.getElementById('fileName').innerText = selectedFile.name + ' (' + Math.round(selectedFile.size / 1024) + ' KB)';
        document.getElementById('uploadBtn').disabled = false;
      }
    }

    function startOTA() {
      if (!selectedFile) return;
      const btn = document.getElementById('uploadBtn');
      const wrap = document.getElementById('progWrap');
      const bar = document.getElementById('barFill');
      const pct = document.getElementById('progPct');
      const status = document.getElementById('statusText');

      btn.disabled = true;
      wrap.style.display = 'block';
      status.style.color = '#00f0ff';
      status.innerText = 'Transferring binary to ESP32-CAM...';

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const percent = Math.round((e.loaded / e.total) * 100);
          bar.style.width = percent + '%';
          pct.innerText = percent + '%';
        }
      };

      xhr.onload = () => {
        if (xhr.status === 200 && xhr.responseText.includes('SUCCESS')) {
          bar.style.width = '100%';
          pct.innerText = '100%';
          status.style.color = '#00ff9d';
          status.innerText = 'SUCCESS! Firmware written. Drone rebooting in 5s...';
          setTimeout(() => { window.location.href = '/'; }, 8000);
        } else {
          status.style.color = '#ff3366';
          status.innerText = 'FLASH FAILED: ' + xhr.responseText;
          btn.disabled = false;
        }
      };

      xhr.onerror = () => {
        status.style.color = '#ffb703';
        status.innerText = 'REBOOTING... Connection closed by drone. Reconnecting...';
        setTimeout(() => { window.location.href = '/'; }, 10000);
      };

      xhr.send(selectedFile);
    }
  </script>
</body>
</html>
)rawliteral";

// 6. Wireless OTA Handlers (Port 80)
static esp_err_t update_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, UPDATE_HTML, strlen(UPDATE_HTML));
}

static esp_err_t update_post_handler(httpd_req_t *req) {
  char buf[1024];
  int remaining = req->content_len;

  if (remaining <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty firmware file");
    return ESP_FAIL;
  }

  // U_FLASH targets the application firmware partition
  if (!Update.begin(remaining, U_FLASH)) {
    char errBuf[64];
    snprintf(errBuf, sizeof(errBuf), "OTA Begin Failed: err %d", Update.getError());
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errBuf);
    return ESP_FAIL;
  }

  while (remaining > 0) {
    int toRead = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
    int received = httpd_req_recv(req, buf, toRead);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      Update.abort();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream receive timeout");
      return ESP_FAIL;
    }

    if (Update.write((uint8_t*)buf, received) != (size_t)received) {
      Update.abort();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write error");
      return ESP_FAIL;
    }
    remaining -= received;
  }

  if (Update.end(true)) {
    if (Update.isFinished()) {
      httpd_resp_sendstr(req, "SUCCESS");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
      return ESP_OK;
    }
  }

  Update.abort();
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware verification failed");
  return ESP_FAIL;
}

// =================== LIVE DIAGNOSTIC LOG ENDPOINT ===================
static esp_err_t logs_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Refresh", "2"); // Auto-refresh browser every 2 seconds

  char outBuf[160];
  int count = logCount;
  int startIdx = (logHead - count + MAX_LOG_LINES) % MAX_LOG_LINES;

  httpd_resp_send_chunk(req, "=== AIR UNIT LIVE DIAGNOSTIC LOG (Auto-Refresh 2s) ===\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "Firmware: v2.2 (ArduinoOTA + Live Logging)\n", HTTPD_RESP_USE_STRLEN);
  snprintf(outBuf, sizeof(outBuf), "FC UART Bytes: %lu | LoRa TX Packets: %lu | Radio Ready: %d\n", fcByteCount, loraPktCount, (int)loraReady);
  httpd_resp_send_chunk(req, outBuf, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "------------------------------------------------------\n", HTTPD_RESP_USE_STRLEN);

  for (int i = 0; i < count; i++) {
    int idx = (startIdx + i) % MAX_LOG_LINES;
    snprintf(outBuf, sizeof(outBuf), "%s\n", logBuffer[idx]);
    httpd_resp_send_chunk(req, outBuf, HTTPD_RESP_USE_STRLEN);
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

// Start Dual-Port Web & Stream Architecture
void startCameraServer() {
  // 1. Port 80 Server: Dedicated to Cockpit UI and Instant Controls
  httpd_config_t config_ui = HTTPD_DEFAULT_CONFIG();
  config_ui.server_port = 80;
  config_ui.ctrl_port = 32768; // Primary control port
  config_ui.max_uri_handlers = 10;
  config_ui.stack_size = 8192; // 8KB stack for smooth flash writes

  httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET,  .handler = index_handler,       .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET,  .handler = capture_handler,     .user_ctx = NULL };
  httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET,  .handler = control_handler,     .user_ctx = NULL };
  httpd_uri_t status_uri  = { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler,      .user_ctx = NULL };
  httpd_uri_t update_get  = { .uri = "/update",  .method = HTTP_GET,  .handler = update_get_handler,  .user_ctx = NULL };
  httpd_uri_t update_post = { .uri = "/update",  .method = HTTP_POST, .handler = update_post_handler, .user_ctx = NULL };
  httpd_uri_t logs_uri    = { .uri = "/logs",    .method = HTTP_GET,  .handler = logs_handler,        .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config_ui) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &update_get);
    httpd_register_uri_handler(camera_httpd, &update_post);
    httpd_register_uri_handler(camera_httpd, &logs_uri);
  }

  // 2. Port 81 Server: Dedicated Exclusively to the MJPEG Stream
  httpd_config_t config_stream = HTTPD_DEFAULT_CONFIG();
  config_stream.server_port = 81;
  config_stream.ctrl_port = 32769; // Distinct secondary control port (avoids socket collision)
  config_stream.max_uri_handlers = 2;
  config_stream.stack_size = 4096;

  httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &config_stream) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

// =================== SETUP ===================
void setup() {
  // Serial0 connects to F405 FC UART at 19200 baud with 2048-byte RX buffer
  Serial.setRxBufferSize(2048);
  Serial.begin(19200);

  // Initialize Flashlight PWM (GPIO 4)
  initFlash();

  // 1. Initialize OV2640 Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Allocate DMA buffers at maximum resolution in PSRAM so runtime switches (VGA to UXGA) always succeed
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t camErr = esp_camera_init(&config);
  if (camErr == ESP_OK) {
    sensor_t * s = esp_camera_sensor_get();
    if (s) {
      s->set_framesize(s, FRAMESIZE_QVGA); // Start at QVGA (320x240) for low-latency FPV
      s->set_quality(s, 12);
    }
  }

  // 2. Initialize Wi-Fi Soft AP & Dual-Port HTTP Server
  WiFi.softAP(AP_SSID, AP_PASS);
  startCameraServer();

  // 3. Hardware pulse reset for Ra-02 (SX1278)
  if (LORA_RST != -1) {
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(10);
    digitalWrite(LORA_RST, HIGH);
    delay(10);
  }

  // Initialize LoRa SPI Bus
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (LoRa.begin(433E6)) {
    loraReady = true;
    LoRa.setSpreadingFactor(7);           // Fast throughput for MAVLink
    LoRa.setSignalBandwidth(250E3);        // 250 kHz bandwidth
    LoRa.setCodingRate4(5);               // 4/5 coding rate
    LoRa.setSyncWord(0x12);                // Private drone sync word
    LoRa.setTxPower(17);                   // +17dBm (50mW) rock-solid without brownouts
    LoRa.setOCP(100);                      // 100mA overcurrent limit protects ESP32-CAM 3V3 rail
  }

  // 4. Pin LoRa bridge to Core 0 so video serving on Core 1 never drops packets
  xTaskCreatePinnedToCore(
    loraBridgeLoop,
    "LoRaBridge",
    4096,
    NULL,
    2,
    &LoRaBridgeTask,
    0
  );

  // 5. Initialize ArduinoOTA for direct one-click wireless flashing from Arduino IDE
  ArduinoOTA.setHostname("esp32-cam");
  ArduinoOTA.begin();

  // 6. Initialize MAVLink UDP Bridge on standard port 14550 for QGC Wi-Fi Direct Connection
  mavUdp.begin(14550);
  addLog("Air Unit Booted. FW: v2.3 (MAVLink UDP 14550 + LoRa)");
}

void loop() {
  ArduinoOTA.handle();
  vTaskDelay(20 / portTICK_PERIOD_MS);
}

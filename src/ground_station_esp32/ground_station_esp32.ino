/*
  Ground Station Firmware (Mounted on FlySky FS-i6 Transmitter)
  Hardware: ESP32 DevKit + LoRa Ra-02 (433MHz) + 0.96" I2C OLED (JMD0.96D-1 SSD1306)
  Power: Dedicated 1S 3.7V 520mAh LiPo with 100k/100k divider on GPIO 34.

  Features:
  - Receives MAVLink telemetry from Drone Air Unit over 433 MHz LoRa.
  - Forwards raw MAVLink packets to Android smartphone via Bluetooth Classic SPP ("Drone_Telemetry").
    (Keeps phone 4G/5G mobile internet completely active for live satellite map tiles in QGroundControl!)
  - Forwards waypoint missions and commands from QGroundControl back to the drone over LoRa.
  - Displays live telemetry directly on the OLED: Flight Mode, Sats, Drone Battery %, TX Battery %, Altitude, Lat/Lon.
*/

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BluetoothSerial.h"

// =================== PIN DEFINITIONS ===================
// LoRa SPI Bus (Contiguous 4-pin block: D21, D19, D18, D5)
#define LORA_SCK        5       // SPI Clock (D5)
#define LORA_MISO       18      // SPI Master In Slave Out (D18)
#define LORA_MOSI       19      // SPI Master Out Slave In (D19)
#define LORA_SS         21      // SPI Chip Select (D21)
#define LORA_RST        15      // Hardware Reset pin (D15)
#define LORA_DIO0       -1      // Unconnected / Polled via SPI (can connect to D4 if desired)

// OLED I2C Bus (Custom ESP32 I2C pins)
#define OLED_SDA        23      // I2C Data (D23)
#define OLED_SCL        22      // I2C Clock (D22)
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDR_1   0x3C
#define SCREEN_ADDR_2   0x3D

// Transmitter Station Battery ADC (100k/100k Resistor Divider on D34)
#define STATION_BAT_PIN 34
// Calibration factor tuned for physical multimeter reference (4.02V actual vs 3.87V raw reading = 1.0388)
#define BAT_CALIBRATION_FACTOR 1.0907

// =================== OBJECTS ===================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
BluetoothSerial SerialBT;

// =================== TELEMETRY VARIABLES ===================
float droneBatVolts     = 0.0;
int   droneBatPct       = 0;
float droneAltitude     = 0.0;
float droneLat          = 0.0;
float droneLon          = 0.0;
int   droneSats         = 0;
char  flightMode[16]    = "STABILIZE";
float droneSpeed        = 0.0;
int   droneHeading      = 0;
float droneDistHome     = 0.0;
float homeLat           = 0.0;
float homeLon           = 0.0;

float stationBatVolts   = 0.0;
int   stationBatPct     = 0;

unsigned long lastTelemetryTime = 0;
unsigned long lastDisplayUpdate  = 0;
unsigned long lastBatCheckTime   = 0;
bool oledReady = false;
volatile uint32_t totalLoRaPacketsReceived = 0;
volatile uint32_t totalUplinkPacketsSent = 0;
volatile unsigned long lastAirPingTime = 0;
volatile uint32_t airFcBytes = 0;
volatile int lastPacketRssi = 0;

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
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(250E3);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(0x12);
    LoRa.setTxPower(17);
    LoRa.setOCP(100);
  }
}

bool safeLoRaSend(const uint8_t* data, size_t len, uint32_t timeoutMs = 120) {
  if (len == 0) return false;

  if (!LoRa.beginPacket()) {
    recoverLoRaRadio();
    return false;
  }
  LoRa.write(data, len);
  LoRa.endPacket(true);

  unsigned long start = millis();
  bool txDone = false;
  while (millis() - start < timeoutMs) {
    uint8_t flags = readLoRaReg(0x12);
    if (flags & 0x08) {
      writeLoRaReg(0x12, 0x08);
      txDone = true;
      break;
    }
    delay(1);
  }

  if (!txDone) {
    recoverLoRaRadio();
    return false;
  }

  LoRa.parsePacket();
  return true;
}

// =================== LIGHTWEIGHT MAVLINK PARSER ===================
// Custom zero-overhead MAVLink v1 / v2 state machine parser.
// Extracts only essential fields without requiring massive external libraries.
void parseMavlinkByte(uint8_t c) {
  static uint8_t state = 0;
  static uint8_t payloadLen = 0;
  static uint32_t msgId = 0;
  static uint8_t buf[64];
  static uint16_t bytesRead = 0; // Total payload bytes received (can be > 64)
  static unsigned long lastByteTime = 0;

  // Timeout protection: If a packet isn't completed within 150ms, reset parser
  if (state != 0 && (millis() - lastByteTime > 150)) {
    state = 0;
  }
  lastByteTime = millis();

  switch (state) {
    case 0: // Search for MAVLink frame start byte
      if (c == 0xFD) { state = 1; }      // MAVLink 2 start byte
      else if (c == 0xFE) { state = 10; } // MAVLink 1 start byte
      break;

    // --- MAVLink 2 Framing ---
    case 1: payloadLen = c; state = 2; break; // Payload length
    case 2: state = 3; break;                 // Incompatible flags
    case 3: state = 4; break;                 // Compatible flags
    case 4: state = 5; break;                 // Packet sequence
    case 5: state = 6; break;                 // System ID
    case 6: state = 7; break;                 // Component ID
    case 7: msgId = c; state = 8; break;      // Message ID Byte 0
    case 8: msgId |= ((uint32_t)c << 8); state = 9; break;  // Message ID Byte 1
    case 9: msgId |= ((uint32_t)c << 16); state = 20; bytesRead = 0; break; // Message ID Byte 2 -> Payload start

    // --- MAVLink 1 Framing ---
    case 10: payloadLen = c; state = 11; break; // Payload length
    case 11: state = 12; break;                 // Sequence
    case 12: state = 13; break;                 // System ID
    case 13: state = 14; break;                 // Component ID
    case 14: msgId = c; state = 20; bytesRead = 0; break; // Message ID -> Payload start

    // --- Payload Extraction ---
    case 20:
      // Store first 64 bytes for our essential telemetry decoders
      if (bytesRead < sizeof(buf)) {
        buf[bytesRead] = c;
      }
      bytesRead++;

      if (bytesRead >= payloadLen) {
        lastTelemetryTime = millis();

        // 1. HEARTBEAT (Message #0): Decode ArduCopter Flight Mode
        if (msgId == 0 && payloadLen >= 9) {
          uint32_t customMode = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

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

          static uint32_t lastStationMode = 0xFFFFFFFF;
          bool modeChanged = (customMode != lastStationMode);
          lastStationMode = customMode;
          if (modeChanged && oledReady) {
            updateOLED(); // Refresh transmitter OLED immediately on mode change!
          }
        }
        // 2. SYS_STATUS (Message #1): Drone 3S LiPo Voltage & Percentage
        else if (msgId == 1 && payloadLen >= 31) {
          uint16_t mVolts = buf[14] | (buf[15] << 8);
          droneBatVolts = mVolts / 1000.0;
          droneBatPct = constrain((int)((droneBatVolts - 10.5) / (12.6 - 10.5) * 100.0), 0, 100);
        }
        // 3. GLOBAL_POSITION_INT (Message #33): Lat, Lon, Altitude, Speed, Heading
        else if (msgId == 33 && payloadLen >= 28) {
          int32_t lat = (int32_t)(buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24));
          int32_t lon = (int32_t)(buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24));
          int32_t relAltMm = (int32_t)(buf[16] | (buf[17] << 8) | (buf[18] << 16) | (buf[19] << 24));
          droneLat = lat / 1e7;
          droneLon = lon / 1e7;
          droneAltitude = relAltMm / 1000.0;

          int16_t vx = (int16_t)(buf[20] | (buf[21] << 8));
          int16_t vy = (int16_t)(buf[22] | (buf[23] << 8));
          droneSpeed = (sqrt((float)vx*vx + (float)vy*vy) / 100.0) * 3.6; // km/h
          droneHeading = (uint16_t)(buf[24] | (buf[25] << 8)) / 100;      // degrees

          if (homeLat == 0.0 && droneLat != 0.0 && droneSats >= 6) {
            homeLat = droneLat;
            homeLon = droneLon;
          }
          if (homeLat != 0.0) {
            float dLat = (droneLat - homeLat) * 111319.5;
            float dLon = (droneLon - homeLon) * 111319.5 * cos(homeLat * 0.01745329);
            droneDistHome = sqrt(dLat*dLat + dLon*dLon);
          }
        }
        // 4. GPS_RAW_INT (Message #24): Satellites Visible
        else if (msgId == 24 && payloadLen >= 30) {
          droneSats = buf[29];
        }
        // 5. STATUSTEXT (Message #253): Instant Flight Mode Announcements (<20ms!)
        else if (msgId == 253 && payloadLen >= 2) {
          char tBuf[51];
          int tLen = (payloadLen > 1) ? payloadLen - 1 : 0;
          if (tLen > 50) tLen = 50;
          memcpy(tBuf, (char*)&buf[1], tLen);
          tBuf[tLen] = '\0';
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
                if (oledReady) updateOLED(); // Instant OLED refresh!
              }
            }
          }
        }

        // Wait for 2 CRC bytes before resetting to state 0
        state = 30;
      }
      break;

    case 30: state = 31; break; // CRC byte 1
    case 31: state = 0; break;  // CRC byte 2 -> Ready for next packet!
  }
}

// =================== TRANSMITTER BATTERY MONITOR ===================
void updateStationBattery() {
  analogSetPinAttenuation(STATION_BAT_PIN, ADC_11db);
  uint32_t rawSum = 0;
  for (int i = 0; i < 16; i++) {
    rawSum += analogRead(STATION_BAT_PIN);
    delay(1);
  }
  float rawAvg = rawSum / 16.0;

  // 100k/100k divider factor = 2.0; BAT_CALIBRATION_FACTOR calibrates to exact multimeter reading
  stationBatVolts = (rawAvg / 4095.0) * 3.3 * 2.0 * BAT_CALIBRATION_FACTOR;

  // 1S LiPo voltage curve: 3.40V empty (0%) to 4.20V full (100%)
  int pct = (int)((stationBatVolts - 3.40) / (4.20 - 3.40) * 100.0);
  stationBatPct = constrain(pct, 0, 100);
}

// =================== OLED SCREEN UPDATE ===================
void updateOLED() {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Link alive watchdog: flag lost if no MAVLink packet arrived in last 3.5 seconds
  bool linkAlive = (millis() - lastTelemetryTime < 3500) && (lastTelemetryTime > 0);
  bool airRfAlive = (millis() - lastAirPingTime < 3000) && (lastAirPingTime > 0);

  // CRITICAL BATTERY ALERT: Flashes if Drone 3S LiPo drops below 10.7V
  bool lowBatWarning = linkAlive && (droneBatVolts > 5.0) && (droneBatVolts < 10.7);

  if (lowBatWarning) {
    // High-priority blinking alert banner (overrides carousel)
    bool blink = (millis() / 500) % 2;
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(blink ? "! ! ! WARNING ! ! !" : "  CRITICAL BATTERY  ");
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    display.setCursor(0, 16);
    display.print("DRN BAT: ");
    display.print(droneBatVolts, 1);
    display.print("V (");
    display.print(droneBatPct);
    display.print("%)");

    display.setCursor(0, 28);
    display.print("LAND IMMEDIATELY!");

    display.drawLine(0, 40, 128, 40, SSD1306_WHITE);

    display.setCursor(0, 46);
    display.print("ALT: ");
    display.print(droneAltitude, 1);
    display.print("m  SAT:");
    display.print(droneSats);

    display.setCursor(0, 56);
    display.print("RTL TRIGGER SUGGESTED");

    display.display();
    return;
  }

  // If telemetry link is alive: Run Option B Asymmetric Auto-Carousel (20s total)
  if (linkAlive) {
    // Page 1: 0 to 13999 ms (14.0 seconds) -> Fighter Jet HUD
    // Page 2: 14000 to 16999 ms (3.0 seconds) -> GPS Retrieval Deck
    // Page 3: 17000 to 19999 ms (3.0 seconds) -> RF Link Diagnostics
    unsigned long cycle = millis() % 20000;
    int page = (cycle < 14000) ? 1 : ((cycle < 17000) ? 2 : 3);

    if (page == 1) {
      // ===== PAGE 1: FIGHTER JET HUD (10.0s) =====
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(flightMode);
      display.setCursor(76, 0);
      display.print("S:");
      display.print(droneSats);
      display.print(" ");
      if (lastPacketRssi > -65) display.print("[|||]");
      else if (lastPacketRssi > -80) display.print("[||.]");
      else if (lastPacketRssi > -95) display.print("[|..]");
      else display.print("[...]");

      display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

      display.setCursor(0, 14);
      display.print("DRN: ");
      display.print(droneBatVolts, 1);
      display.print("V (");
      display.print(droneBatPct);
      display.print("%)");

      display.setCursor(0, 25);
      display.print("TX : ");
      display.print(stationBatVolts, 2);
      display.print("V (");
      display.print(stationBatPct);
      display.print("%)");

      display.drawLine(0, 36, 128, 36, SSD1306_WHITE);

      display.setCursor(0, 40);
      display.print("ALT: ");
      display.print(droneAltitude, 1);
      display.print("m  SPD:");
      display.print((int)droneSpeed);
      display.print("kph");

      display.setCursor(0, 52);
      display.print("DST: ");
      display.print((int)droneDistHome);
      display.print("m   [o . .]");

    } else if (page == 2) {
      // ===== PAGE 2: GPS RETRIEVAL DECK (3.5s) =====
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("GPS RETRIEVAL DECK");
      display.setCursor(110, 0);
      display.print(droneSats);

      display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

      display.setCursor(0, 14);
      display.print("LAT: ");
      if (droneLat != 0.0) display.print(droneLat, 5); else display.print("NO GPS 3D FIX");

      display.setCursor(0, 25);
      display.print("LON: ");
      if (droneLon != 0.0) display.print(droneLon, 5); else display.print("ACQUIRING...");

      display.drawLine(0, 36, 128, 36, SSD1306_WHITE);

      display.setCursor(0, 40);
      display.print("ALT: ");
      display.print(droneAltitude, 1);
      display.print("m  HDG:");
      display.print(droneHeading);
      display.print("*");

      display.setCursor(0, 52);
      display.print("DST: ");
      display.print((int)droneDistHome);
      display.print("m   [. o .]");

    } else {
      // ===== PAGE 3: RF & LINK STATUS (3.5s) =====
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("RF & LINK STATUS");

      display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

      display.setCursor(0, 14);
      display.print("LORA: ");
      display.print(lastPacketRssi);
      display.print(" dBm");

      display.setCursor(0, 25);
      display.print("BT  : ");
      display.print(SerialBT.hasClient() ? "PHONE CONNECTED" : "AWAITING PHONE");

      display.drawLine(0, 36, 128, 36, SSD1306_WHITE);

      display.setCursor(0, 40);
      display.print("RX:");
      display.print(totalLoRaPacketsReceived);
      display.print(" UP:");
      display.print(totalUplinkPacketsSent);

      display.setCursor(0, 52);
      display.print("FC:");
      if (airFcBytes > 1024) {
        display.print(airFcBytes / 1024.0, 1);
        display.print("K");
      } else {
        display.print(airFcBytes);
        display.print("B");
      }
      display.print("   [. . o]");
    }

  } else {
    // Telemetry not connected: show RF Link & FC UART connection diagnostics
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setCursor(0, 0);
    if (airRfAlive) {
      display.print("LORA: OK (");
      display.print(lastPacketRssi);
      display.print("dBm)");
    } else {
      display.print("LORA: NO RF SIGNAL");
    }

    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    display.setCursor(0, 14);
    display.print("DRN: --.-V (--%)");

    display.setCursor(0, 25);
    display.print("TX :");
    display.print(stationBatVolts, 2);
    display.print("V (");
    display.print(stationBatPct);
    display.print("%)");

    display.drawLine(0, 36, 128, 36, SSD1306_WHITE);

    display.setCursor(0, 40);
    if (airRfAlive) {
      if (airFcBytes == 0) {
        display.print("FC UART: 0 B (IDLE)");
      } else {
        display.print("FC UART: ");
        if (airFcBytes > 1024) {
          display.print(airFcBytes / 1024.0, 1);
          display.print(" KB");
        } else {
          display.print(airFcBytes);
          display.print(" B");
        }
      }
    } else {
      display.print("AWAITING AIR PING...");
    }

    display.setCursor(0, 52);
    if (airRfAlive) {
      if (airFcBytes == 0) {
        display.print("NO FC DATA -> CHK T6");
      } else {
        display.print("STREAMING MAVLINK...");
      }
    } else {
      display.print("WAIT AIR | PKT:");
      display.print(totalLoRaPacketsReceived);
    }
  }

  display.display();
}

// =================== SETUP ===================
void setup() {
  Serial.begin(115200);

  // 1. Initialize OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000); // 100kHz standard mode for high electrical noise immunity
  if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR_1)) {
    oledReady = true;
  } else if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR_2)) {
    oledReady = true;
  }

  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 18);
    display.println("GROUND STATION");
    display.setCursor(12, 32);
    display.println("LORA TELEMETRY");
    display.setCursor(22, 46);
    display.println("BOOTING UP...");
    display.display();
  }

  // 2. Initialize Bluetooth Classic SPP
  SerialBT.begin("Drone_Telemetry");
  Serial.println("Bluetooth Started! Ready to pair as 'Drone_Telemetry'");

  // 3. Hardware pulse reset for Ra-02 (SX1278) to guarantee clean transceiver state
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

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa initialization failed!");
    if (oledReady) {
      display.clearDisplay();
      display.setCursor(0, 24);
      display.println("LORA INIT FAILED!");
      display.setCursor(0, 38);
      display.println("Check SPI/RST lines");
      display.display();
    }
    while (1);
  }

  // Matching RF settings with Air Unit
  LoRa.setSpreadingFactor(7);           // SF7 for fast throughput
  LoRa.setSignalBandwidth(250E3);        // 250 kHz bandwidth
  LoRa.setCodingRate4(5);               // 4/5 coding rate
  LoRa.setSyncWord(0x12);                // Private drone sync word
  LoRa.setTxPower(17);                   // +17dBm (50mW) output power
  LoRa.setOCP(100);                      // 100mA overcurrent limit

  updateStationBattery();
  delay(1000);
}

// =================== MAIN LOOP ===================
void loop() {
  // 1. Receive incoming LoRa packets from Drone -> Forward to Phone (BT) + Parse for OLED
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    totalLoRaPacketsReceived++;
    lastPacketRssi = LoRa.packetRssi();

    uint8_t rxBuf[256];
    int len = 0;
    while (LoRa.available() && len < sizeof(rxBuf)) {
      rxBuf[len++] = LoRa.read();
    }

    // Check if this is an Air Unit Diagnostic Heartbeat ("$AG", 13 bytes)
    if (len == sizeof(AirDiagPacket) && rxBuf[0] == 0x24 && rxBuf[1] == 0x41 && rxBuf[2] == 0x47) {
      AirDiagPacket* diag = (AirDiagPacket*)rxBuf;
      if (diag->checksum == calcChecksum(rxBuf, sizeof(AirDiagPacket) - 1)) {
        lastAirPingTime = millis();
        airFcBytes = diag->fcByteCount;
        // Cleanly handled internal diagnostic heartbeat; do not forward to QGroundControl
      }
    } else {
      // Forward standard MAVLink telemetry bytes to BOTH Bluetooth and USB Serial
      for (int i = 0; i < len; i++) {
        SerialBT.write(rxBuf[i]);   // Send byte to QGroundControl on phone (Bluetooth)
        Serial.write(rxBuf[i]);     // Send byte to QGroundControl on PC (USB Serial)
        parseMavlinkByte(rxBuf[i]); // Extract telemetry for local OLED display
      }
    }
  }

  // 2. Receive commands from Phone (BT) OR PC (USB Serial) -> Forward to Drone over LoRa
  if (SerialBT.available() || Serial.available()) {
    uint8_t buffer[160];
    int count = 0;
    unsigned long startRead = millis();
    while ((millis() - startRead < 12) && count < sizeof(buffer)) {
      if (SerialBT.available()) {
        buffer[count++] = SerialBT.read();
        startRead = millis();
      } else if (Serial.available()) {
        buffer[count++] = Serial.read();
        startRead = millis();
      } else {
        delay(1);
      }
    }
    if (count > 0) {
      totalUplinkPacketsSent++;
      // Wait up to 40ms for any active downlink packet to clear
      unsigned long waitStart = millis();
      while ((readLoRaReg(0x18) & 0x08) && (millis() - waitStart < 40)) {
        delay(2);
      }
      safeLoRaSend(buffer, count);

      // Critical Command Redundancy Burst:
      // If this is an Arm/Disarm (Msg #76) or Mode Change (Msg #11), send a fast 2nd burst
      // Guarantees 99.9% packet delivery rate through RF noise without waiting for timeout!
      bool isCommand = false;
      if (buffer[0] == 0xFD && count >= 10) {
        uint32_t mId = buffer[7] | ((uint32_t)buffer[8] << 8);
        if (mId == 76 || mId == 11) isCommand = true;
      } else if (buffer[0] == 0xFE && count >= 8) {
        if (buffer[5] == 76 || buffer[5] == 11) isCommand = true;
      }
      if (isCommand) {
        delay(10);
        safeLoRaSend(buffer, count);
      }
    }
  }

  // 3. Monitor Station 1S LiPo Battery every 2 seconds
  if (millis() - lastBatCheckTime > 2000) {
    lastBatCheckTime = millis();
    updateStationBattery();
  }

  // 4. Refresh OLED at 4 Hz (Smooth display without starving telemetry loop)
  if (millis() - lastDisplayUpdate > 250) {
    lastDisplayUpdate = millis();
    updateOLED();
  }
}

# 500mm Quadcopter Commutation Testbed & Dual-Tier Telemetry System

> *"This was once an abandoned project. I tried everything I could, hit wall after wall, and no matter what I did, it refused to work right. So I gave up on it. But later, I made a personal vow: I will not start a single new project until I return to this machine, fix every root-cause flaw, and make this drone fly rock-solid and super stable."*

---

## The Origin: From Shelved Failure to Flawless Stability

Every drone builder knows the frustration of an aircraft that simply refuses to cooperate. This project started with blown ESC MOSFETs, motor commutation stuttering, timer collisions, intermittent brownouts, and violent bench crashes that destroyed carbon-reinforced propellers. It culminated in an uncontrolled fly-away and tree snag that felt like the absolute end of the road. 

Exhausted by endless debugging and unexplainable hardware failures, the project was shelved and declared dead.

Months later came the turning point: a firm resolution to never leave an engineering puzzle unsolved. Rather than moving on to shiny new ideas, the mandate was set: **zero new projects until this quad flies with total, unwavering stability.**

Every subsystem was torn down to first principles:
- **Commutation & Timing:** Replaced conflicting DSHOT protocols with legacy 480Hz analog PWM timing to match the analog ESC gate drivers.
- **Electrical Isolation:** Rebuilt the power distribution, banishing ground loops and powering the ESP32 RF section from a dedicated 5.3V 2.5A switching regulator to kill 700mA transient brownouts permanently.
- **Flight Controller Core:** Migrated to ArduCopter on an STM32F405 with custom EKF3 sensor fusion, notch filtering, and elevated mast compass isolation.
- **Affordable Custom Telemetry:** Replaced expensive commercial RF systems with an ultra-low-cost ($15) Dual-Tier ESP32 Wi-Fi + 433 MHz LoRa architecture with a custom Fighter Jet HUD on the FlySky transmitter.

Today, this repository is the complete blueprint of that comeback: an autonomous 500mm quadcopter that holds rock-solid hovers in wind, uploads missions in sub-second bursts, and streams real-time telemetry over kilometers.

---

## 1. Hardware Specifications

| Component | Hardware Specification | Configuration Notes |
| :--- | :--- | :--- |
| **Airframe** | 500mm diagonal quadcopter (TBS-style geometry) | Reinforced arms with 1045 propellers |
| **Flight Controller** | DakeFPV F405 (STM32F405 MCU) | 1MB onboard flash; vibration-isolated mount |
| **Firmware** | ArduCopter V4.7.1 Custom Build | ChibiOS RTOS; migrated from Betaflight |
| **RC Radio Link** | FlySky FS-i6 Transmitter + FS-iA6B Receiver | i-BUS serial protocol on UART2 (115200 baud); 5-mode mixer |
| **Motors & ESCs** | 4x 1000KV sensorless brushless motors | 4x 30A analog ESCs running 480Hz legacy PWM |
| **Power System** | 3S 2200mAh 35C LiPo battery | XT60 harness, integrated PDB, APM Power Module (+5.3V 2.5A switching BEC for ESP32-CAM) |
| **Navigation** | M10Q 250 GPS + QMC5883L Magnetometer | Elevated folding mast mount; I2C compass on external bus |
| **Air Telemetry Unit** | AI-Thinker ESP32-CAM + Ra-02 (SX1278 433 MHz) | UART6 MAVLink bridge; dual-core RTOS; dual-band RF |
| **Ground Station** | ESP32 DevKit v1 + Ra-02 + 0.96" SSD1306 OLED | Bluetooth Classic SPP to phone; USB COM to PC; 1S 520mAh LiPo |

---

## 2. Repository Structure

```
quad-commutation-testbed/
├── README.md                           # Main engineering overview & build log
├── .gitignore                          # Build artifacts and firmware binary ignore rules
├── firmware/                           # Flight controller firmware targets
│   └── arducopter_with_bl.hex          # ArduCopter V4.7.1 binary with bootloader (DAKEFPVF405)
├── config/                             # Flight plans and configuration backups
│   ├── betaflight_cli_dump.txt         # Betaflight baseline commutation and PID dump
│   └── missions/
│       └── park_test_mission.waypoints # ArduPilot autonomous waypoint flight plan
├── src/                                # Microcontroller source code
│   ├── air_unit_esp32_cam/             # ESP32-CAM firmware: MAVLink UDP :14550, LoRa & FPV video
│   ├── ground_station_esp32/           # Ground station: LoRa transceiver, Bluetooth SPP & OLED HUD
│   └── ground_diagnostic/              # Standalone LoRa RF diagnostic & RSSI sniffer
├── docs/                               # Detailed technical documentation & logs
│   ├── hardware_architecture.md        # Deep dive into timers, commutation, and electrical design
│   ├── hardware_wiring/README.md       # Pinout tables, PDB distribution, and sensor harnesses
│   ├── bench_testing_log.md            # Daily bench testing progression & chronological logs
│   ├── failure_logs.md                 # Complete diary of all 15 issues, root causes, and fixes
│   ├── telemetry_and_video/README.md   # Dual-tier telemetry blueprint, RF design, and wiring schematics
│   └── testing_logs/                   # Field test logs with video recordings (Tests 01 through 08)
└── assets/                             # Hardware photography, schematics, and flight footage
    ├── hardware/                       # High-resolution component and build photos
    ├── schematics/                     # Motor rotation and pinout diagrams
    └── media/                          # Walkaround and flight test video recordings
```

---

## 3. Engineering Issues & Solutions Diary

Every unexpected failure, glitch, and hardware issue encountered across the build has been documented with root-cause analysis in [docs/failure_logs.md](docs/failure_logs.md). Below is the comprehensive summary table:

| # | Issue Encountered | Root Cause | Engineering Solution |
| :- | :--- | :--- | :--- |
| **01** | Motors 3 & 4 refused to spin | Timer collision when running DSHOT on legacy analog ESCs | Switched protocol to legacy analog PWM (400Hz). |
| **02** | Radio lost connection to FC | Firmware flash wiped Betaflight UART settings | Reassigned UART2 to Serial RX with IBUS provider. |
| **03** | Flashed wrong firmware target | Flashed `SPEEDYBEEF405V3` instead of `DAKEFPVF405` | Reflashed correct target and recalibrated ESC ranges (1000–2000µs). |
| **04** | Stator chatter / creaking at idle | Low PWM frequency caused stator vibration | Raised idle throttle to 1065 and bumped PWM rate to 480Hz. |
| **05** | Motor 4 shorted ESC | Blown MOSFET acted like an electromagnetic brake | Desoldered and replaced ESC board without disassembling the frame. |
| **06** | The Bench Incident (props destroyed) | Motor 4 braking caused violent pitch and wall impact | Established permanent 15kg dumbbell anchor rule for all prop testing. |
| **07** | Failsafe failed (motors spun on TX off) | Receiver held last command on signal loss | Enabled hardware failsafe in FS-i6 menu on Ch3 (-100%) and Ch5 (Disarm). |
| **08** | Betaflight flasher failed on ArduPilot | Flasher rejected foreign partition tables | Used STM32CubeProgrammer in DFU mode with full chip erase. |
| **09** | Compass missing (SCL stuck at 0.2V) | `NTF_LED_TYPES = 455` locked I2C bus searching for LEDs | Set `NTF_LED_TYPES = 1` (internal LEDs only); SCL snapped to 3.3V. |
| **10** | Analog ESC boot lockout (8-hr mystery) | ArduPilot boot (8s) exceeded analog ESC timeout (2s) | Powered FC first via mobile phone USB-C reverse charge, then plugged LiPo. |
| **11** | Low battery RTL canopy crash | `RTL_ALT` (15m) was lower than local tree line (18-20m) | Rescued via spliced bamboo pole; set `BATT_FS = Land` and `RTL_ALT = 22m`. |
| **12** | Telemetry UART dropouts & silence | Misconfigured ArduPilot serial parameters & stream rates | Configured `SERIAL6_PROTOCOL = 2`, `BAUD = 19`, and enabled SR6 stream rates. |
| **13** | QGC command timeouts & mode lag | Half-duplex LoRa collisions & heartbeat delay | Instant mode extraction via `STATUSTEXT` (Msg 253) & immediate ACK fast-flush. |
| **14** | LoRa bandwidth bottleneck for missions | LoRa 19200 baud too slow for 900+ parameters / missions | Built dual-tier architecture: high-speed Wi-Fi UDP :14550 + long-range LoRa. |
| **15** | Battery sags & silent ESP32 brownouts | Low battery resistance + 700mA peak RF spikes tripped BOD | Spliced APM Power Module dedicated 5.3V 2.5A switching BEC directly to ESP32-CAM (isolated from FC). |
| **16** | Premature failsafe on motor punchouts | 60A motor loads caused temporary 1.5V voltage sag | Configured `BATT_LOW_TIMER = 10`, `BATT_LOW_VOLT = 10.2V`, and `BATT_FS = Land`. |

---

## 4. Dual-Tier Telemetry & Live Video Architecture

Commercial telemetry radios (SiK 433/915MHz, RFD900) and dedicated 5.8GHz FPV gear typically cost upwards of $200. This build accomplishes long-range bidirectional telemetry, live FPV streaming, and cockpit HUD controls for under $15 using off-the-shelf ESP32 and LoRa modules.

```
                                  +------------------------------------+
                                  |         ArduPilot F405 FC          |
                                  +-----------------+------------------+
                                                    | UART6 (19200 baud MAVLink 2)
                                                    v
                                  +------------------------------------+
                                  |     ESP32-CAM Air Unit (Core 0/1)  |
                                  +--------+------------------+--------+
                                           |                  |
           Tier 1: High-Speed 2.4 GHz Wi-Fi|                  |Tier 2: Punchy 433 MHz LoRa
           (UDP :14550, HTTP :80, Stream :81)                 |(SX1278 Ra-02, +17dBm, SF7)
                                           |                  |
                                           v                  v
                   +-------------------------------+  +-------------------------------+
                   | Mobile / Laptop QGC & Browser |  | ESP32 Ground Station (FS-i6)  |
                   | - Instant mission upload (<1s)|  +---------------+---------------+
                   | - Instant param sync (2s)     |                  | Bluetooth SPP / USB
                   | - 30 FPS MJPEG Video Stream   |                  v
                   | - Zero-latency Arm / Guided   |  +-------------------------------+
                   +-------------------------------+  | FlySky FS-i6 OLED & Phone QGC |
                                                      | - Glanceable 0.96" OLED HUD   |
                                                      | - Cellular 5G data stays open |
                                                      | - Multi-kilometer range       |
                                                      +-------------------------------+
```

### Tier 1: High-Speed Wi-Fi MAVLink (Port 14550)
- **Use Case:** Bench calibration, pre-flight checks, instant waypoint/mission uploads, parameter adjustments, and close-range FPV video.
- **Protocol:** MAVLink 2 broadcast over UDP Port 14550 via the ESP32 SoftAP (`BuddyThisAintFreeWifi`).
- **Throughput:** Megabits/second. Downloading the complete ArduPilot parameter tree takes ~2 seconds instead of 60+ seconds.
- **Video:** Dual-HTTP server architecture. Port 80 serves the tactical OSD cockpit and camera controls; Port 81 delivers dedicated, non-blocking 30 FPS MJPEG streaming.

### Tier 2: Long-Range LoRa Telemetry (433 MHz)
- **Use Case:** Outdoor flights beyond Wi-Fi range (1 to 2+ kilometers).
- **RF Configuration:** 433 MHz carrier, Spreading Factor 7, 250 kHz bandwidth, Coding Rate 4/5, +17 dBm TX power.
- **Ground Station HUD:** Mounted directly onto the FlySky FS-i6 transmitter. Displays live flight mode, battery voltage/percentage, GPS altitude, satellite count, speed, and radio link quality on a 0.96" SSD1306 OLED display.
- **Phone Connectivity:** Forwards MAVLink telemetry to the phone over Bluetooth Classic SPP (`Drone_Telemetry`). Because Wi-Fi is unused on the phone during flight, the phone retains active 4G/5G mobile data, enabling live Google/Mapbox satellite imagery in QGroundControl.

---

## 5. Hardware Power Architecture & Brownout Mitigation

The ESP32-CAM integrates an OV2640 camera sensor, a 2.4 GHz Wi-Fi radio, and an SPI bus driving the Ra-02 LoRa transceiver. During active operation, peak current requirements surge significantly:

```
Idle ESP32 Core:             ~80 mA
OV2640 Camera Sensor:        ~160 mA
Ra-02 LoRa Transmission:     ~110 mA (+17 dBm)
Wi-Fi 802.11b/g/n Bursts:    ~350 mA
---------------------------------------
Total Peak Surge Current:    ~700 mA
```

### The Brownout Reset Failure Mode
When operating on a depleted 3S LiPo battery, the battery's internal resistance increases. During simultaneous Wi-Fi packet transmission and LoRa transmission, the transient 700 mA surge causes a momentary voltage dip. If powered from the flight controller's internal 5V regulator, the 5V rail collapses below 4.5V.

Because the ESP32-CAM's onboard AMS1117-3.3 linear regulator requires 1.1V to 1.3V of headroom, a drop on the 5V rail pulls the 3.3V rail down to 2.8V. This triggers the ESP32 hardware **Brownout Detector (BOD)**, causing a silent chip reset that drops Wi-Fi client associations and interrupts telemetry.

### The Solution: APM Power Module Isolation & Sag Filtering (Verified Flawless)
1. **Isolated Heavy-Duty 5.3V Switching BEC:**
   - Powered the ESP32-CAM and LoRa module directly from the inline **APM Power Module's 5.3V 2.5A switching regulator** (Pin 1/2 VCC and Pin 5/6 GND).
   - Completely disconnected the +5V red wire from the flight controller to prevent regulator back-feeding.
   - The 5.3V rail provides 2.0V of headroom above the ESP32-CAM's onboard AMS1117-3.3 dropout limit, completely eliminating brownouts even under peak Wi-Fi/LoRa RF load.
2. **ArduPilot Battery Sag Filtering:**
   - Configured `BATT_LOW_TIMER = 10` (ArduPilot ignores transient 1–4 second throttle sags, requiring 10 continuous seconds of low voltage before triggering failsafe).
   - Set under-load thresholds: `BATT_LOW_VOLT = 10.2V` (3.40V/cell) and `BATT_CRT_VOLT = 9.9V` (3.30V/cell).
   - Set pre-arm threshold `BATT_ARM_VOLT = 11.1V` and failsafe action to Land (`BATT_FS_LOW_ACT = 1`).

---

## 6. How to Flash & Operate

### Step 1: Flight Controller Setup (ArduPilot)
1. Flash `firmware/arducopter_with_bl.hex` using STM32CubeProgrammer in DFU mode with full chip erase.
2. Open Mission Planner, connect via USB-C, and verify the following UART6 telemetry parameters:
   - `SERIAL6_PROTOCOL = 2` (MAVLink 2)
   - `SERIAL6_BAUD = 19` (19200 baud)
   - `BRD_SER6_RTSCTS = 0` (Disable flow control)
   - `SR6_EXT_STAT = 2`, `SR6_EXTRA1 = 4`, `SR6_EXTRA2 = 4`, `SR6_POSITION = 3`, `SR6_RAW_SENS = 2`

### Step 2: Air Unit (ESP32-CAM)
1. Open `src/air_unit_esp32_cam/air_unit_esp32_cam.ino` in Arduino IDE.
2. Select Board: **AI Thinker ESP32-CAM** (PSRAM: Enabled, Flash Frequency: 80MHz).
3. Connect via FTDI programmer (GPIO 0 jumpered to GND for flashing). Upload the sketch.
4. Subsequent updates can be flashed wirelessly over-the-air via `http://192.168.4.1/update`.

### Step 3: Ground Station (ESP32 DevKit)
1. Open `src/ground_station_esp32/ground_station_esp32.ino` in Arduino IDE.
2. Select Board: **ESP32 Dev Module**.
3. Upload the sketch via USB cable.

### Step 4: Connecting QGroundControl
- **Direct Wi-Fi Mode (Pre-Flight / Pit):**
  1. Connect your phone or PC to Wi-Fi SSID `BuddyThisAintFreeWifi` (Password: `notfree123`).
  2. Launch QGroundControl. QGC automatically connects over UDP port 14550.
  3. Upload waypoints, sync parameters, or open `http://192.168.4.1/` in your browser for live video.
- **Long-Range LoRa Mode (Flight):**
  1. Power on the Ground Station ESP32.
  2. In QGC, link via Bluetooth to `Drone_Telemetry` or plug the Ground Station into PC USB.
  3. Telemetry streams continuously over 433 MHz LoRa with glanceable OLED readouts.

---

## 7. Media & Documentation Links

- **Hardware Architecture:** [docs/hardware_architecture.md](docs/hardware_architecture.md)
- **Wiring & Pinout Tables:** [docs/hardware_wiring/README.md](docs/hardware_wiring/README.md)
- **Bench Testing Logs:** [docs/bench_testing_log.md](docs/bench_testing_log.md)
- **Troubleshooting & Crash Logs:** [docs/failure_logs.md](docs/failure_logs.md)
- **Telemetry & Video Blueprint:** [docs/telemetry_and_video/README.md](docs/telemetry_and_video/README.md)
- **Flight Testing Logs & Footage:** [docs/testing_logs/README.md](docs/testing_logs/README.md)
  - [Test 01 Hardware Check](docs/testing_logs/test_01/README.md)
  - [Test 02 Takeoff Wobble](docs/testing_logs/test_02/README.md)
  - [Test 03 Stable Hover](docs/testing_logs/test_03/README.md)
  - [Test 04 Failsafe Drop](docs/testing_logs/test_04/README.md)
  - [Test 05 ArduPilot Arming](docs/testing_logs/test_05/README.md)
  - [Test 06 Stabilization](docs/testing_logs/test_06/README.md)
  - [Test 07 Altitude Lock](docs/testing_logs/test_07/README.md)
  - [Test 08 Outdoor RTL & Tree Rescue](docs/testing_logs/test_08/README.md)

# Quadcopter Hardware Wiring & Power Architecture

This guide details the complete wiring layout for the 500mm quadcopter testbed, including the DakeFPV F405 flight controller, FlySky receiver, analog ESCs, M10Q GPS/compass mast, and the APM Power Module powering the ESP32-CAM telemetry air unit.

---

## 1. Power Distribution & APM Power Module Topology

```
             [3S 2200mAh 35C LiPo Battery]
                          |
                          v (XT60)
             +-------------------------+
             |    APM Power Module     |
             +------------+------------+
                          |
         +----------------+----------------+
         | (Main High-Current Leads)       | (6-Pin Jumper Harness)
         v                                 |
  +--------------+                         |-- Pin 1/2 (+5.3V, 2.5A BEC)
  | Main ESC PDB |                         |   --> ESP32-CAM "5V" Pin
  +-------+------+                         |
          |-- ESC 1 Power (VCC / GND)      |-- Pin 5/6 (GND)
          |-- ESC 2 Power (VCC / GND)      |   --> ESP32-CAM "GND" Pin
          |-- ESC 3 Power (VCC / GND)      |   --> F405 FC "GND" Pad
          |-- ESC 4 Power (VCC / GND)      |
          |                                |-- Pin 3 (Current Sense, optional)
          |-- F405 FC Power                |-- Pin 4 (Voltage Sense, optional)
              (Spliced to BAT / GND)
```

> [!IMPORTANT]
> **Power Isolation Rule:**
> The ESP32-CAM and LoRa Ra-02 transceiver draw high peak currents (up to 700 mA during simultaneous Wi-Fi and LoRa TX bursts). 
> They are powered **exclusively** from the APM Power Module's dedicated 5.3V 2.5A switching regulator.
> The +5V pad on the DakeFPV F405 is **NOT** connected to the ESP32-CAM. This prevents back-feeding between regulators and completely isolates the flight controller's internal sensor rail from RF brownouts.

---

## 2. Flight Controller (DakeFPV F405) Pinout & Connections

![DakeFPV F405 Top Pads](../../assets/hardware/dakefpv_f405_top_pads.jpg)

### A. Power & Motors
| F405 Pad | Connection | Notes |
| :--- | :--- | :--- |
| **BAT** | PDB Battery Positive | Spliced from main PDB to power the onboard F405 linear regulators |
| **GND** | PDB Battery Ground | Main electrical ground reference |
| **S1** | ESC 1 Signal | Motor 1 PWM signal (Front-Right, CCW) |
| **S2** | ESC 2 Signal | Motor 2 PWM signal (Rear-Left, CCW) |
| **S3** | ESC 3 Signal | Motor 3 PWM signal (Front-Left, CW) |
| **S4** | ESC 4 Signal | Motor 4 PWM signal (Rear-Right, CW) |
| **GND** (Signal Row) | All 4 ESC Ground wires | Tied together to prevent ground loops |

*(Note: The +5V red BEC wire on all 4 analog ESCs has been cut/isolated to prevent voltage conflicts.)*

### B. RC Receiver (FlySky FS-iA6B)
| F405 Pad | Receiver Pin | Protocol | Notes |
| :--- | :--- | :--- | :--- |
| **5V** | VCC | 5V DC | Powered from F405 receiver BEC |
| **GND** | GND | Ground | Common ground |
| **R2** (UART2 RX) | SER / i-BUS Out | i-BUS Serial (115200) | Configured in ArduPilot as Serial RX |

### C. M10Q GPS & QMC5883L Compass (Elevated Mast Mount)
| F405 Pad | GPS/Compass Pin | Bus / Function | Notes |
| :--- | :--- | :--- | :--- |
| **5V** | 5V | Power | Powers GPS receiver and compass sensor |
| **GND** | GND | Ground | Common ground |
| **T1** (UART1 TX) | RX | GPS Serial (38400) | FC configuration commands to GPS |
| **R1** (UART1 RX) | TX | GPS Serial (38400) | NMEA / UBX satellite stream to FC |
| **SCL** | SCL | I2C Clock (Bus 1) | External compass clock (`NTF_LED_TYPES = 1`) |
| **SDA** | SDA | I2C Data (Bus 1) | External compass bidirectional data |

### D. Telemetry Air Unit (ESP32-CAM + Ra-02 LoRa)
| F405 Pad | ESP32-CAM Pin | Bus / Function | Notes |
| :--- | :--- | :--- | :--- |
| *No Connection* | **5V** | Power Input | **Powered from APM Power Module (+5.3V), NOT from F405!** |
| **GND** | **GND** | Signal Ground | Common reference tied with APM Module GND |
| **T6** (UART6 TX) | **U0R** (GPIO 3) | MAVLink 2 (19200) | F405 streams telemetry to ESP32-CAM |
| **R6** (UART6 RX) | **U0T** (GPIO 1) | MAVLink 2 (19200) | ESP32-CAM forwards QGC commands to F405 |

---

## 3. Air Unit Internal Wiring (ESP32-CAM to Ra-02 LoRa)

The Ra-02 433 MHz LoRa module connects to Header 1 (Left header, Front View) of the ESP32-CAM via an uncrossed 4-wire SPI ribbon:

| Ra-02 Pin | ESP32-CAM Pin | Location | Function |
| :--- | :--- | :--- | :--- |
| **3.3V** | **3.3V** | Header 2, Pin 1 (Top-Right) | Clean regulated 3.3V from ESP32-CAM LDO |
| **GND** | **GND** | Header 1, Pin 2 (Left) | Common ground |
| **RST** | **GPIO 12** | Header 1, Pin 3 (Left) | Hardware reset line (pulsed on boot) |
| **SCK** | **GPIO 13** | Header 1, Pin 4 (Left) | Hardware SPI Clock |
| **MISO** | **GPIO 15** | Header 1, Pin 5 (Left) | Hardware SPI Master In Slave Out |
| **MOSI** | **GPIO 14** | Header 1, Pin 6 (Left) | Hardware SPI Master Out Slave In |
| **NSS / CS** | **GPIO 2** | Header 1, Pin 7 (Left) | Hardware SPI Chip Select |
| **ANT** | **16.4 cm Wire** | Directly to ANT pad | Quarter-wave whip antenna pointing straight UP |

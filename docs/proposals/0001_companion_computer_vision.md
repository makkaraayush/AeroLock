# RFC-001: Raspberry Pi Companion Computer & Edge AI Engine

**Status:** `APPROVED`  
**Category:** Autonomy / Computer Vision  
**Author:** Lead Engineer & Antigravity  
**Created:** Oct 15, 2026  
**Approved:** Oct 15, 2026  

---

## 1. Executive Summary & Motivation

The 500mm quadcopter testbed currently runs ArduCopter on an STM32F405 flight controller with basic GPS navigation (RTL, Alt-Hold, Stabilize). While the F405 excels at real-time control loops and sensor fusion at 400 Hz, its embedded ARM Cortex-M4 microcontroller has zero capacity to process imagery, run neural networks, or perform high-level robotic decision making.

This proposal blueprints the integration of a **Linux companion computer (Raspberry Pi)** directly aboard the 500mm airframe, connected to the flight controller over a high-speed MAVLink UART bridge. The ESP32 is preserved as a dedicated long-range RF telemetry bridge.

---

## 2. Division of Labor (System Roles)

```
+-----------------------------------------------------------------------------------+
|                                  500mm AIRFRAME                                   |
|                                                                                   |
|  +--------------------+         +--------------------+     +--------------------+ |
|  |   RASPBERRY PI     |         |   DAKEFPV F405 FC  |     |  ESP32-CAM AIR     | |
|  | (Edge AI & Vision) |         | (Real-time Flight) |     | (Telemetry Bridge) | |
|  |                    |         |                    |     |                    | |
|  | - OpenCV Tracking  | MAVLink | - 400Hz PID Loops  | UART| - Wi-Fi UDP :14550 | |
|  | - Follow-Me Logic  |<=======>| - EKF3 Navigation  |<===>| - 433 MHz LoRa     | |
|  | - AprilTag Landing | UART 4  | - Motor PWM Drive  |UART6| - Tactical Web HUD | |
|  | - Target Precision | (921k)  | - Hardware Safety  |     |                    | |
|  +--------------------+         +--------------------+     +--------------------+ |
|           ^                              ^                           ^            |
|           | 5V 3A Buck                   | APM Power Mod             | APM Power  |
+-----------|------------------------------|---------------------------|------------+
            |                              |                           |
            +------------------------------+---------------------------+
                                           |
                                  +--------+--------+
                                  | 3S 2200mAh LiPo |
                                  +-----------------+
```

1. **DakeFPV F405 (Hard Real-Time):** Focuses 100% of CPU cycles on IMU sampling, EKF attitude estimation, analog motor PWM, and emergency hardware failsafes.
2. **Raspberry Pi (High-Level Autonomy):** Runs Linux, camera capture pipeline, OpenCV object detection, and emits high-level MAVLink navigation targets (`SET_POSITION_TARGET_LOCAL_NED`) into ArduPilot in `GUIDED` mode.
3. **ESP32-CAM (Telemetry Bridge):** Handles ground communication (Wi-Fi UDP port 14550 + 433 MHz LoRa to the FlySky transmitter OLED). It does not bear the CPU burden of computer vision.

---

## 3. Hardware Selection & Comparison

| Parameter | Raspberry Pi Zero 2 W | Raspberry Pi 4B (2GB/4GB) | Raspberry Pi 5 (4GB) |
| :--- | :--- | :--- | :--- |
| **Form Factor / Weight** | **16 grams** (Ultra-lightweight) | 46 grams | 46 grams + active cooler |
| **Processor** | Quad-core Cortex-A53 @ 1.0 GHz | Quad-core Cortex-A72 @ 1.5 GHz | Quad-core Cortex-A76 @ 2.4 GHz |
| **RAM** | 512 MB LPDDR2 | 2 GB / 4 GB LPDDR4 | 4 GB LPDDR4X |
| **Camera Interface** | Single CSI ribbon (Mini) | Full-size CSI ribbon | Dual 4-lane CSI |
| **OpenCV Performance** | ~12–18 FPS (QVGA/VGA tracking) | ~25–35 FPS (Full VGA tracking) | ~60+ FPS (YOLOv8-nano capable) |
| **Power Consumption** | **1.2W – 2.5W** (0.25A – 0.5A @ 5V) | 3.5W – 6.0W (0.7A – 1.2A @ 5V) | 5.0W – 12.0W (Heavy thermal) |
| **Thermal Output** | Low (Heatsink only) | Medium (Small fan needed) | High (Active cooler mandatory) |
| **Recommendation** | **Best for Budget & Weight** | **Best Balance for AI Vision** | Overkill for initial testbed |

**Recommendation:** 
- Start with **Raspberry Pi Zero 2 W** or **Raspberry Pi 4B**. 
- The 500mm frame has ample payload capacity (>500g headroom with 1045 props and 1000KV motors). The Pi 4B provides USB 3.0 ports, 2GB–4GB RAM, and handles standard OpenCV algorithms without hitting RAM bottlenecks.

---

## 4. Crazy Next-Level Feature Proposals

### Feature A: Dynamic Visual "Follow-Me" Tracking
- **How it works:** Pi captures video via a Raspberry Pi Camera Module (or USB wide-angle webcam).
- A lightweight OpenCV tracker (CSRT or KCF) locks onto a target (the pilot, a colored marker, or a vehicle).
- The Pi calculates horizontal error ($dx$) and vertical scale change ($ds$).
- It translates image coordinates into body-frame velocity vectors:
  - $V_x$ (Forward/Back): Keeps a constant 3-meter distance based on target bounding box area.
  - $V_y$ (Left/Right): Keeps target centered in the frame.
  - $Yaw\_Rate$: Rotates the drone in place to maintain line of sight.
- Sends MAVLink `SET_POSITION_TARGET_LOCAL_NED` messages at 15 Hz in ArduPilot `GUIDED` mode.

### Feature B: Millimeter-Precision AprilTag Landing Pad
- **How it works:** A downward-facing 120-degree camera looks at the landing area.
- Ground pad has an AprilTag (Family `tag36h11`) printed on weatherproof cardstock.
- During RTL, ArduPilot navigates via GPS to within ~2 meters of Home.
- At 4 meters altitude, the Pi acquires the AprilTag, calculates 6-DOF relative pose ($X, Y, Z, Roll, Pitch, Yaw$), and issues precision vector corrections (`LANDING_TARGET` MAVLink message #149).
- The drone touches down dead-center on the target pad with millimeter accuracy, completely ignoring GPS drift!

### Feature C: Optical Flow for GPS-Denied Indoor Hover
- **How it works:** Downward camera paired with a cheap VL53L1X Time-of-Flight (ToF) laser rangefinder ($3).
- Uses OpenCV Lucas-Kanade optical flow on floor textures to compute ground velocity $(\Delta x, \Delta y)$ scaled by laser altitude.
- Injects velocity estimates into ArduPilot EKF3 via `VISION_POSITION_ESTIMATE`.
- Quad hovers rock-solid in a closed room, basement, or garage with **zero GPS satellites**!

### Feature D: Gesture Flight Control
- **How it works:** MediaPipe or OpenCV Haar cascades detect hand poses.
  - Open Palm = Hold Position / Loiter.
  - Point Left / Right = Translate laterally.
  - Thumbs Up = Climb 1 meter.
  - Wave Both Hands = Return to Launch.

### Feature E: Cloud 4G LTE Link (Unlimited Global Telemetry)
- Plug a cheap 4G USB dongle or SIM7600 cellular HAT into the Raspberry Pi.
- Joins a secure private ZeroTier / Tailscale virtual network.
- Stream MAVLink telemetry and live HD video across cellular networks with zero distance limit.

---

## 5. Electrical & Wiring Architecture

```
3S LiPo (11.1V - 12.6V)
       |
       +---> [ Dedicated 5V 3A Buck Converter (XL4015) ] ---> Raspberry Pi (5V / GND)
       |
       +---> [ APM Power Module (5.3V 2.5A BEC) ] ----------> DakeFPV F405 & ESP32-CAM

F405 Flight Controller (UART 4 / Telem 2):
  - TX4 (GPIO PA0)  ------->  Raspberry Pi RX (GPIO 15 / Pin 10)
  - RX4 (GPIO PA1)  <-------  Raspberry Pi TX (GPIO 14 / Pin 8)
  - GND             <=======> Common Ground
```

### Critical Electrical Safeguards
1. **Dedicated Power Rail:** The Raspberry Pi must **never** be powered from the F405 flight controller 5V rail. A Pi 4B drawing up to 1.5A under heavy OpenCV load will instantly brown out the flight controller. Use an isolated 5V 3A buck converter.
2. **Logic Levels:** Both the STM32F405 and the Raspberry Pi GPIO headers operate at 3.3V logic. No logic level converters are required on UART 4.
3. **Serial Baud Rate:** Set `SERIAL4_BAUD = 921` (921,600 baud) and `SERIAL4_PROTOCOL = 2` (MAVLink 2) for ultra-low latency inter-processor messaging.

---

## 6. Software Stack

1. **OS:** Raspberry Pi OS Lite (64-bit, headless, no desktop GUI for minimum RAM and CPU overhead).
2. **Communication Daemon:** `MAVProxy` or lightweight custom `asyncio` Python script using `pymavlink`.
3. **Vision Pipeline:** Python 3.11 + OpenCV (`opencv-python-headless`) + NumPy.
4. **Safety Failsafes:**
   - Configure ArduPilot `FS_GCS_ENABLE = 1`.
   - If the Raspberry Pi Python script crashes or freezes, it stops sending the 2 Hz heartbeat.
   - ArduPilot detects companion heartbeat loss after 3 seconds and automatically falls back to `LOITER` or `RTL` under manual pilot control.

---

## 7. Approval & Status

- [x] Technical Feasibility Reviewed
- [x] Power Budget Verified
- [x] Pilot Sign-off (Approved by Lead Pilot Oct 15, 2026)
- [x] Prototyping Authorized

---

## 8. Bill of Materials & Procurement Links (Robu.in Single-Basket)

| Item | Component | Specification | Estimated Cost | Direct Store Link |
| :--- | :--- | :--- | :--- | :--- |
| 1 | **Raspberry Pi Zero 2 W** | Quad-core 64-bit Cortex-A53, 512MB RAM | ~₹1,500 | [Robu.in Product Page](https://robu.in/product/raspberry-pi-zero-2-w/) |
| 2 | **5MP Camera + Cable** | OV5647 5MP with Pi Zero FFC cable included | ~₹520 | [Robu.in Product Page](https://robu.in/product/5mp-raspberry-pi-zero-w-camera-module-w-hbv-ffc-cable/) |
| 3 | **3A DC Buck Converter** | Mini MP1584EN (steps 3S LiPo down to 5.1V) | ~₹65 | [Robu.in Product Page](https://robu.in/product/mini-mp1584-dc-dc-3a-adjustable-buck-module/) |
| 4 | **Collision Avoidance ToF** | GY-53 VL53L0X Laser Distance Sensor (I2C) | ~₹579 | [Robu.in Product Page](https://robu.in/product/gy-53-vl53l0x-laser-tof-distance-ranging-sensor/) |
| 5 | **Jumper Wires** | Female-to-Female 20cm Dupont lines (40 pcs) | ~₹55 | [Robu.in Product Page](https://robu.in/product/female-to-female-dupont-line-20cm-40-pin/) |
| 6 | **Pi Zero Heatsink** | Mini aluminum adhesive heatsink | ~₹30 | [Robu.in Product Page](https://robu.in/product/aluminum-heatsink-for-raspberry-pi-zero/) |
| 7 | **40-Pin Header Strip** | 2.54mm male breakaway pin strip (if bare Pi) | ~₹15 | [Robu.in Product Page](https://robu.in/product/40-pin-2-54-mm-male-berg-strip-breakaway-header-straight/) |
| 8 | **MicroSD Card (Amazon)** | SanDisk Ultra 32GB Class 10 A1 (Cheaper on Amazon) | ~₹350 | [Amazon India Search](https://www.amazon.in/s?k=SanDisk+Ultra+32GB+micro+sd) |

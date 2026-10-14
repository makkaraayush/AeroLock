# RFC-003: Advanced Autonomy & Hardware Expansion Roadmap

**Status:** `IDEA`  
**Category:** Hardware Expansion & Sensors  
**Author:** Lead Engineer & Antigravity  
**Created:** Oct 15, 2026  

---

## 1. Overview

This document serves as the high-voltage ideas reservoir for future capabilities on the 500mm quadcopter testbed. Ideas listed here are categorized by complexity, hardware requirements, and potential impact.

---

## 2. Advanced Feature Brainstorm

### Idea 1: Forward & Downward LiDAR (Terrain Follow & Obstacle Avoidance)
- **Target Hardware:** Benewake TFmini-Plus ($35) or VL53L1X I2C ToF Laser ($4).
- **Concept:** 
  - Downward LiDAR: Replaces barometer for ultra-precise ground tracking (keeps drone exactly 1.0m above slopes, grass, and uneven terrain).
  - Forward LiDAR: Detects obstacles (trees, walls, poles) up to 12 meters ahead. ArduPilot `PRX_TYPE` / `AVOID_ENABLE` automatically stops or diverts the drone before collision.
- **Flight Controller Integration:** Connects directly via I2C or UART on the DakeFPV F405.

### Idea 2: Autonomous Payload Release Mechanism
- **Target Hardware:** 9g micro servo (SG90) + 3D printed quick-release hook pin.
- **Concept:** 
  - Allows air-dropping small payloads, emergency medical kits, or markers.
  - Triggerable via:
    1. FlySky transmitter switch (AUX channel mapped in ArduPilot `SERVOx_FUNCTION = 19`).
    2. QGroundControl mission waypoint command (`DO_SET_SERVO`).
    3. Automated computer vision trigger (e.g. drop when AprilTag target is centered).

### Idea 3: Dynamic Moving "Follow-Home" (Return-to-Pilot)
- **Target Hardware:** Smartphone / Ground Station GPS.
- **Concept:** 
  - Standard RTL returns to the static takeoff point. If the pilot is walking, hiking, or on a boat, the drone flies back to where you started, not where you are.
  - In ArduPilot, activate `GEO_FENCE` and MAVLink dynamic home updates. Ground Station pushes phone GPS coordinates to ArduPilot periodically so RTL always flies directly to your current hand position!

### Idea 4: Current Shunt & Real-Time mAh Consumption Monitoring
- **Target Hardware:** INA226 I2C current sensor or APM Power Module current sense wire.
- **Concept:** 
  - Connect the unused `CURR` pin of the inline APM Power Module to the F405 flight controller `CURR` ADC pad.
  - Calibrate `BATT_AMP_PERVOLT`. ArduPilot will calculate exact current draw (Amperes) and integrate consumed milliamp-hours (mAh).
  - Telemetry and OLED will display remaining mAh alongside battery voltage, giving 10x more accurate battery state-of-charge than voltage alone.

### Idea 5: Dual-Axis Micro Pan/Tilt Camera Gimbal
- **Target Hardware:** Two 9g metal-gear servos (MG90S) + 3D printed lightweight bracket.
- **Concept:**
  - Pilot can look around during flight using the FlySky VR knobs (VRA for pitch, VRB for roll/yaw).
  - When combined with the Raspberry Pi companion computer, the gimbal can automatically track objects independently of drone yaw rotation!

### Idea 6: 4G LTE Cloud Telemetry & Unlimited Range
- **Target Hardware:** Huawei E3372 4G USB Dongle or Waveshare SIM7600G-H LTE HAT.
- **Concept:**
  - Connects companion computer to standard mobile 4G/5G networks.
  - Joins a private WireGuard or ZeroTier virtual private network.
  - Control the drone and watch live video from home or anywhere in the world with zero RF range limits.

---

## 3. Evaluation & Priority Matrix

| Feature | Complexity | Cost | Safety Impact | Recommended Order |
| :--- | :--- | :--- | :--- | :--- |
| **APM Current Shunt (mAh Tracking)** | Low | $0 (Already wired) | High (Prevents battery exhaustion) | **1 (Immediate)** |
| **Raspberry Pi + Camera Follow-Me** | Medium | ~$30–$45 | Medium (High fun/autonomy) | **2 (Next Major Phase)** |
| **Downward Laser Rangefinder** | Low | ~$4–$15 | High (Precision indoor hover) | **3** |
| **AprilTag Precision Landing** | Medium | $0 (Uses Pi camera) | High (Eliminates landing drift) | **4** |
| **Payload Release Mechanism** | Low | ~$2 (9g servo) | Low (Mission versatility) | **5** |
| **4G LTE Cellular Link** | High | ~$30–$50 | Medium (Requires SIM plan) | **6** |

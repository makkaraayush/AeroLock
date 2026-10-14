# RFC-002: DroneBridge & ESP-NOW vs. 433 MHz LoRa Telemetry Evaluation

**Status:** `DEFERRED` (Retained 433 MHz LoRa & Wi-Fi UDP architecture)  
**Category:** RF & Communications Architecture  
**Author:** Lead Engineer & Antigravity  
**Created:** Oct 15, 2026  

---

## 1. Executive Summary

This proposal evaluates the technical feasibility of replacing or augmenting the current 433 MHz Ra-02 (SX1278) LoRa telemetry link with **DroneBridge for ESP32** utilizing **ESP-NOW** (Espressif's connectionless 2.4 GHz protocol).

The goal is to determine whether switching to ESP-NOW provides superior range and throughput, and whether it eliminates the need for LoRa hardware on the 500mm quadcopter testbed.

---

## 2. Technical Comparison Matrix

| Evaluation Parameter | 433 MHz LoRa (Current) | DroneBridge / ESP-NOW (2.4 GHz) | Direct Wi-Fi UDP :14550 (Current) |
| :--- | :--- | :--- | :--- |
| **RF Carrier Frequency** | **433 MHz** (Sub-GHz UHF) | **2.4 GHz** (Microwave ISM) | **2.4 GHz** (Microwave ISM) |
| **RF Wavelength ($\lambda$)** | **69.3 cm** | 12.5 cm | 12.5 cm |
| **Maximum Bandwidth** | ~2.5 KB/s | **~100–150 KB/s** | **Megabits/s (Ultra-wide)** |
| **Real-World Range (Line of Sight)** | **1 to 3+ km** | ~500m to 800m (with tuned antennas) | ~50m to 80m |
| **Obstacle & Canopy Penetration** | **Exceptional** (Diffracts around trees and terrain) | **Poor** (Heavily absorbed by foliage water molecules) | **Poor** |
| **Interference with FlySky 2.4G RC** | **ZERO interference** (Completely orthogonal band) | **HIGH RISK of desensing FlySky receiver** | Moderate (Local launchpad only) |
| **Telemetry Latency** | ~25–40 ms | **~5–10 ms** | **<1 ms** |
| **Hardware Complexity** | Needs external SPI radio (Ra-02) | **Zero extra hardware** (Native on ESP32) | **Zero extra hardware** |
| **Transmitter OLED HUD Support** | **Custom Fighter Jet HUD on FlySky** | Requires raw serial / external app | Browser / QGC |

---

## 3. Deep Engineering Analysis: Key Roadblocks

### Hazard 1: 2.4 GHz Co-Location & Receiver Desensing (The FlySky Conflict)
The FlySky FS-i6 transmitter and FS-iA6B receiver communicate using the **AFHDS 2A protocol on 2.4 GHz**.
- On a 500mm quadcopter, the flight electronics are closely packed. If an ESP32 runs ESP-NOW, its onboard 2.4 GHz radio transmits telemetry bursts at **+20 dBm (100 mW)** just inches away from the FlySky antennas.
- **Receiver Desensing:** Strong in-band RF transmission overwhelms the Low-Noise Amplifier (LNA) on the FS-iA6B receiver. Even with frequency hopping, this significantly degrades your manual RC control link, increases packet drops, and can trigger premature RC failsafes.
- **The LoRa Advantage:** 433 MHz is in the sub-GHz UHF band. Its harmonics do not interfere with 2.4 GHz. Your FlySky RC control and 433 MHz LoRa run simultaneously with 100% mutual RF isolation.

### Hazard 2: Foliage & Tree Penetration Physics
Recall the **Oct 06 tree incident** documented in [failure_logs.md](../failure_logs.md) (Issue 11):
- A 2.4 GHz radio wave has a wavelength of only **12.5 cm**. Water molecules resonate and absorb microwave radiation around 2.4 GHz. Dense tree branches, leaves, and wet canopy attenuate 2.4 GHz signals aggressively (10 dB to 20 dB per tree cluster).
- A 433 MHz radio wave has a **69.3 cm wavelength**. Sub-GHz waves easily bend (diffract) around tree trunks, foliage, and obstacles. Even with the drone wedged high in a 20-meter canopy, the Ground Station was able to track RF signal without loss.

---

## 4. What DroneBridge for ESP32 Actually Provides

DroneBridge for ESP32 (open-source by Wolfgang Christl) operates in two main modes:
1. **Wi-Fi AP / Client Mode:** Provides a standard MAVLink UDP bridge on port 14550 and an HTTP configuration interface.
   - *Note:* **Our custom firmware already provides this natively**, including our dedicated dual-port video architecture, auto-baud detection, and fast-flush waypoint transfer.
2. **ESP-NOW Mode:** Transmits raw connectionless packets between two ESP32 chips without Wi-Fi association overhead.
   - Requires two ESP32 chips: one on the drone and one on the ground station.
   - Delivers higher throughput than LoRa (~100 KB/s), allowing faster parameter sync over medium ranges (~300m–500m).

---

## 5. Architectural Recommendation: The Multi-Tier Verdict

Rather than abandoning LoRa, the optimal architecture is a **Hybrid Multi-Tier Setup**:

```
[ Tier 1: Local Launchpad (<80m) ]
  -> High-Speed Wi-Fi UDP (Port 14550) on ESP32-CAM.
  -> Used for sub-second mission uploads, parameter trees, and live FPV video.

[ Tier 2: Mid-Range Autonomous Flight (80m - 500m) ]
  -> Optional ESP-NOW bridge mode for high-rate telemetry if 2.4GHz antenna separation is verified.

[ Tier 3: Long-Range / Beyond-Visual-Line-of-Sight (>500m - 3km) ]
  -> 433 MHz LoRa (SX1278).
  -> Unbeatable penetration through trees, terrain, and high interference.
  -> Directly powers the FlySky OLED Fighter Jet HUD with zero RC receiver interference.
```

### Next Steps for Evaluation
1. Bench test ESP-NOW signal power alongside the FS-iA6B receiver to measure signal-to-noise ratio (SNR) degradation on the FlySky radio.
2. Keep the current 433 MHz LoRa link active as the safety failsafe telemetry backbone.

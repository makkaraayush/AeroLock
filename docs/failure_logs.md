# What Broke & How I Fixed It

This is where I'm keeping track of all the weird issues I ran into during the upgrade, so I don't go crazy trying to remember how I fixed them!

### Issue 1: Two motors just wouldn't spin
- **What happened:** Motors 1 and 2 worked fine, but 3 and 4 were completely dead.
- **The fix:** Turned out there was a timer collision because the STM32F405 was trying to run DSHOT. I just switched it back to legacy analog PWM (400Hz) and they woke up!

### Issue 2: Radio stopped talking to the drone
- **What happened:** After a firmware flash, I completely lost my FlySky IBUS connection.
- **The fix:** I accidentally wiped the UART settings. I just had to go back into Betaflight and reassign UART2 to Serial RX.

### Issue 3: Flashed the wrong firmware (oops)
- **What happened:** I accidentally flashed the `SPEEDYBEEF405V3` target instead of `DAKEFPVF405`. 
- **The fix:** Re-flashed it with the right target, recalibrated all the ESCs together (2000µs to 1000µs), and now all four motors start up perfectly at the same time. 

### Issue 4: Motors were making a weird creaking sound
- **What happened:** When armed at idle, all the motors were making this weird groaning/creaking noise.
- **The fix:** The low PWM frequency (400Hz) was causing some stator chatter. I raised the idle throttle just a tiny bit (to 1065) and bumped the PWM rate to 480Hz. It runs super smooth now!

### Issue 5: The cursed Motor 4 (Shorted ESC)
- **What happened:** Motor 4 would spin freely when off, but as soon as I plugged the battery in and dropped throttle, it felt like it had the brakes slammed on. 
- **The fix:** Confirmed it was a shorted MOSFET in the ESC that was acting like an electromagnetic brake. 

### Issue 6: The Bench Incident (RIP Propellers)
- **What happened:** I was testing on the bench (not tied down) in ANGLE mode. I gave it a tiny bit of throttle, the PID loop freaked out (windup), and when I dropped the throttle, Airmode kept trying to balance the drone. Because Motor 4's ESC was braking hard, it pitched violently, flew into the wall, and destroyed my propellers.
- **The fix:** Lesson learned: **ALWAYS tie it down to the 15kg dumbbell anchor when testing with props on!**

### Crash Repair Update!
**Status: Success!**
The new soldering strategy worked perfectly. I desoldered the wires from the old ESC board and connected them directly to the new one without taking the whole frame apart. 
- The new ESC is running flawlessly.
- No more electromagnetic braking/shorting issues on Motor 4!
- The motor is spinning in the correct direction.

Next up: Calibrating all the ESCs together and starting the tuning process!

### Issue 7: Failsafe Failed (Motors Kept Spinning on Signal Loss)
- **What happened:** I did a test where I turned off the remote to see if the drone would disarm. Instead, the motors kept right on spinning! Total flyaway risk.
- **The fix:** I had to go into the transmitter's own menu and explicitly turn on the failsafe. I set Channel 3 (throttle) to -100% and Channel 5 (arming switch) to the disarm position. Now, it correctly disarms exactly 1.5 seconds after signal loss (which matches Betaflight's config).

### Issue 8: Betaflight Configurator Failed to Flash ArduPilot Hex
- **What happened:** Tried to flash `arducopter_with_bl.hex` using Betaflight Configurator's "Load Firmware [Local]" tab, but the flash process kept erroring out and refusing to write. Betaflight's flasher is designed specifically for Betaflight firmware and balks at external bootloaders and full-chip hex layouts.
- **The fix:** Switched to **STM32CubeProgrammer**. Booted the STM32F405 into DFU mode, loaded the hex, and flashed it with a full chip erase. It wrote and verified cleanly on the first try.

### Issue 9: M10Q-5883 Compass Undetected (SCL Voltage Drop / LED_TYPE Bus Conflict)
- **What happened:** After installing ArduPilot and wiring up the M10Q-5883 unit, the GPS locked satellites and worked fine, but the compass was completely missing in Mission Planner. Tweaked all compass parameters (`COMPASS_ENABLE`, `COMPASS_AUTO_ROT`, bus IDs), but nothing brought it to life.
- **Diagnostics with Multimeter:**
  - Checked resistance across the lines: readings were normal (no dead short to ground).
  - Unpowered behavior: With the GPS plugged into the FC but unpowered, both SDA and SCL floated around ~1.5V.
  - Powered behavior: Once the GPS was supplied with 5V, SDA immediately pulled up to a healthy 3.3V, but SCL remained stuck down (around 0.2V).
  - The Isolation Test: Desoldered the SCL wire from the FC pad. The loose SCL wire from the GPS immediately jumped up to a solid **3.3V**, and the FC pad showed zero shorts to ground. This proved the GPS module, pull-up resistors, and wiring harness were 100% healthy—the issue was purely inside ArduPilot!
- **The Root Cause:** Parameter collision! `NTF_LED_TYPES` (or `LED_TYPE`) was set to `455`, which instructed ArduPilot to initialize external I2C LED drivers on boot. Because no external I2C LED hardware was connected, the driver grabbed and locked up the I2C bus, dragging the SCL clock line down to 0.2V.
- **The Fix:** Changed the parameter to **1** (Built-in / Internal Board LEDs only). Rebooted the board, the I2C bus was instantly released, SCL snapped up to 3.3V, and the compass was detected immediately in Mission Planner!

### Issue 10: Analog ESCs Refusing to Start on ArduPilot Boot (The 8-Hour Mystery)
- **What happened:** After flashing ArduPilot, plugging in the LiPo battery resulted in completely dead/silent ESCs. They refused to arm or play their startup beeps because they weren't receiving the initial zero-throttle PWM signal on power-up. Yet, strangely enough, if the flight controller was already plugged in and powered via USB first, plugging in the LiPo afterwards made all four ESCs start and run flawlessly!
- **The 8-Hour Grind:** Spent 8 hours nonstop changing every parameter imaginable in Mission Planner, rewiring connections, testing solder joints, and pulling hair out trying to understand why it worked with USB but failed with standalone battery power.
- **The Mobile Charger Breakthrough:** To test a theory about boot timing, I used a standard mobile phone charger to power the FC's 5V input first. I waited a few seconds for ArduPilot to finish its full startup sequence, and then plugged in the LiPo to power the ESCs. Boom—all four ESCs immediately sang their initialization chimes and armed perfectly!
- **The Root Cause:** Hardware timer mismatch between legacy analog ESCs and ArduPilot's bootloader:
  - These older 30A analog ESCs have a strict hardware timeout window on boot. If they don't detect a valid PWM throttle signal within a couple seconds of receiving battery power, their firmware assumes a signal fault and enters a protective lockout state to prevent runaway props.
  - Betaflight never had this issue because it is an ultra-lightweight firmware that boots in milliseconds, immediately spitting out 480Hz PWM pulses before the ESC timeout expires.
  - ArduPilot, on the other hand, runs a full ChibiOS real-time operating system with extensive pre-arm checks, sensor initialization, and EKF filtering that takes roughly 4 to 8 seconds to boot before enabling motor timer outputs.
  - When plugging in the LiPo directly, the ESCs wake up, wait for a signal that isn't ready yet, time out, and shut down before ArduPilot ever finishes booting!
- **The Field Workaround (The Phone Trick):** Instead of cutting and splicing battery wires or adding physical switches just to bypass these cheap old ESCs, I found a way cleaner field solution:
  1. Plug a standard USB Type-C to Type-C cable from my mobile phone directly into the F405 flight controller's USB-C port.
  2. The phone powers the FC via reverse charging, letting ArduPilot complete its full boot sequence and start spitting out PWM pulses.
  3. Wait a few seconds until the boot tones finish, then plug in the main 3S LiPo battery. The ESCs hear the active PWM signal instantly and initialize cleanly!
  4. Unplug the Type-C cable from the phone/FC (the FC stays powered through the onboard BEC from the LiPo). Ready to fly!
  - It's a slightly annoying extra step caused by running ancient legacy ESCs on a shoestring budget, but it completely solves the problem with zero extra hardware or messy wiring until I can upgrade to modern BLHeli_S/32 ESCs down the line.

### Issue 11: Low Battery RTL Triggered into Canopy & The High-Tree Rescue Mission
- **What happened:** Tested ArduPilot Return-To-Launch (RTL) mode twice outdoors. Both trials worked flawlessly: the drone rose autonomously to its 15m target altitude, flew back, and positioned itself directly over the exact takeoff point. However, while bringing the drone back home after testing, the low battery failsafe triggered (`BATT_FS_LOW_ACT` was configured to RTL). The moment it reached home, ArduPilot commanded an autonomous RTL back towards the original launch coordinates. En route at 15m altitude, it flew directly into the canopy of the tallest tree in the neighborhood and became firmly wedged in the upper branches.
- **The Rescue Mission Saga:**
  1. *Climbing the Tree:* Rushed over and climbed up the tree trunk to try shaking the branch or pushing the drone down. Two friends on the ground stretched out a wide cloth underneath to catch the drone and absorb impact. However, the upper branches near the 15m apex were far too narrow and flimsy to support body weight without snapping.
  2. *The Tree Branch Extension:* While perched high in the tree, broke off a long dead branch from the tree itself and tried jabbing at the drone, but it was still just out of reach.
  3. *Calling Emergency Services:* Called the local Fire Brigade for assistance with a ladder, but they stated their policy only permits deployment for life-threatening human emergencies, not drone rescues.
  4. *Engineering an Ultra-Long Rescue Pole:* Climbed down and engineered a rigid rescue pole on the spot: spliced an extremely long bamboo pole together with a lightweight PVC pipe, and lashed a wooden stiffening stick along the joint with tight ties so it wouldn't bow or snap under gravity.
  5. *The Push and the Diving Catch:* Raised the pole all the way up into the canopy. The two helpers maneuvered the pole to push the drone free while I held the safety cloth below. When the drone broke free, its fall trajectory drifted completely away from the cloth! Reacting on pure adrenaline, dropped the cloth, sprinted underneath the falling quad, and caught it mid-air!
- **Damage Assessment:** Incredibly, only a single propeller snapped during the catch. The frame, carbon arms, motors, ESCs, flight controller, GPS mast, and all sensors (IMU, Baro, Compass) survived completely intact!
- **Root Cause & Engineering Dilemma:**
  - `RTL_ALT` was set to the default 15m (1500cm), which is slightly lower than mature neighborhood tree crowns (~18-20m).
  - When the 3S 2200mAh pack hit low voltage failsafe on the walk home, the FC did exactly what it was programmed to do: climbed to 15m and flew a straight vector to the Home GPS coordinates, oblivious to obstacle height.
- **The Permanent Fix & Parameter Adjustments:**
  1. **Low-Battery Failsafe Set to Land (`BATT_FS_LOW_ACT = 1`):** Changed the low battery failsafe from RTL to Land. If battery voltage drops critically, the drone now executes a smooth vertical descent on the spot instead of climbing or attempting cross-neighborhood transit on dying cells.
  2. **RTL Return Altitude Raised to 22m (`RTL_ALT = 2200`):** For intentional, switch-activated Return-To-Launch, the return cruise altitude was increased from 15m to 22m (2200cm). This permanently guarantees the quad cruises safely above the tallest local tree canopies (~18-20m), eliminating any future tree collision hazard during autonomous return flights.

### Issue 12: MAVLink Telemetry UART Dropouts & Flight Controller Silence
- **What happened:** After wiring the ESP32-CAM to UART6 on the DakeFPV F405, the ESP32-CAM was either receiving zero bytes or intermittent garbage from the flight controller. The Web Cockpit HUD showed 0 bytes received from FC, and no telemetry reached the Ground Station.
- **Diagnostics:**
  - Tested direct USB-C connection to PC: Mission Planner received telemetry perfectly over USB, confirming ArduPilot was alive.
  - Inspected UART6 hardware: Checked solder joints on TX6 and RX6. Continuity was good, but no data frames were being received by the ESP32-CAM.
- **The Root Cause:**
  - `SERIAL6_PROTOCOL` in ArduPilot was not set to MAVLink 2 (`SERIAL6_PROTOCOL = 2`).
  - `SERIAL6_BAUD` defaulted to 57 (57600 baud), while the ESP32-CAM was listening at 19200 baud to match the LoRa airtime throughput.
  - Hardware flow control (`BRD_SER6_RTSCTS`) was enabled or floating, stalling the serial transmit buffer because the ESP32-CAM does not break out RTS/CTS pins.
  - ArduPilot default stream rates on unassigned serial ports were set to 0 Hz, meaning ArduPilot was not actively streaming telemetry streams without explicit requests.
- **The Fix:**
  - Configured ArduPilot serial parameters in Mission Planner:
    - `SERIAL6_PROTOCOL = 2` (MAVLink 2)
    - `SERIAL6_BAUD = 19` (19200 baud)
    - `BRD_SER6_RTSCTS = 0` (Disable hardware flow control)
    - `SR6_EXT_STAT = 2` (2 Hz system status)
    - `SR6_EXTRA1 = 4` (4 Hz attitude & mode)
    - `SR6_EXTRA2 = 4` (4 Hz VFR HUD)
    - `SR6_POSITION = 3` (3 Hz GPS position)
    - `SR6_RAW_SENS = 2` (2 Hz sensor health)
  - Telemetry bytes began streaming continuously into the ESP32-CAM at ~1.5 KB/s.

### Issue 13: "Vehicle 1 Did Not Respond to Command: Arm/Disarm" & Mode Lag
- **What happened:** When commanding Arm/Disarm or switching to Guided mode from QGroundControl via Bluetooth, QGC frequently displayed an error toast: "Vehicle 1 did not respond to command: Arm/Disarm". Clicking the button multiple times with brief pauses occasionally worked, but often timed out. In addition, changing flight modes from the RC transmitter took several seconds to reflect on the Ground Station OLED, and occasionally misidentified valid modes as "CUSTOM".
- **Diagnostics:**
  - Direct USB cable test from PC to F405: Arming and mode changes responded instantly in QGC, confirming the flight controller itself was not rejecting commands.
  - LoRa packet tracing: The Ground Station was sending MAVLink `COMMAND_LONG` (Msg #76) across the LoRa link. The Air Unit received it and passed it to the F405 via UART6. The F405 executed the arm command and returned `COMMAND_ACK` (Msg #77). However, QGC timed out waiting for the acknowledgment.
- **The Root Cause:**
  1. *Half-Duplex Radio Collisions:* The SX1278 LoRa transceiver cannot transmit and receive at the same time. At SF7 / 250kHz, sending a command takes ~35 ms. When commands were transmitted, the Ground Station fired back-to-back bursts. Meanwhile, the F405 responded with `COMMAND_ACK` within 10 ms. Because the Ground Station was actively in transmit mode sending the second burst, its receiver was disabled, clobbering the incoming `COMMAND_ACK` packet.
  2. *Mode Detection Latency & CUSTOM Fallback:* The firmware previously relied solely on the 1 Hz `HEARTBEAT` message (Msg #0) to update the flight mode. If a heartbeat was dropped over RF, the mode display was delayed by multiple seconds. Furthermore, the mode decoder had an unhandled default switch case that actively overwrote valid flight modes with "CUSTOM" whenever an unmapped mode ID or malformed packet was received.
- **The Fix:**
  1. *Instant Mode Extraction via STATUSTEXT (Msg #253):* ArduPilot immediately broadcasts a `STATUSTEXT` text toast ("Mode <MODE_NAME>") whenever the flight mode changes. Added regex-style parsing in both the Air Unit and Ground Station firmware to extract the mode string directly from Msg #253. Mode switches now display on the OLED and Web HUD in under 20 ms.
  2. *Protected Mode Table:* Expanded the ArduCopter mode switch to cover all 28 official modes, and removed the destructive "CUSTOM" overwrite in the default handler.
  3. *Immediate ACK Prioritization & Managed Redundancy:* Configured the Air Unit to detect `COMMAND_ACK` (Msg #77) and break out of its receive window immediately (`fastFlushRequested = true`), pushing the acknowledgment over LoRa and Wi-Fi without waiting for staging buffers to fill. On the Ground Station, tuned the uplink burst timing so the radio enters receive mode immediately after transmission.

### Issue 14: LoRa Bandwidth Bottleneck for Heavy Tasks (Dual-Tier Link Architecture)
- **What happened:** While 433 MHz LoRa is unbeatable for long-range outdoor telemetry, it operates at a narrow effective data rate (~1.5 to 2.0 KB/s). When attempting heavy ground station operations like downloading the entire ArduPilot parameter list (over 900 parameters) or uploading multi-waypoint autonomous mission plans, LoRa took up to a minute or dropped packets under heavy traffic.
- **The Engineering Solution:** Implemented a **Dual-Tier Link Architecture**:
  - *Tier 1 (High-Speed Local Wi-Fi):* Added a native MAVLink UDP bridge on port 14550 directly into the ESP32-CAM Air Unit firmware. When the drone is near the operator (pre-flight bench, pit, launch pad), the operator connects their phone or laptop directly to the ESP32-CAM Wi-Fi hotspot (`BuddyThisAintFreeWifi`). QGroundControl automatically links via UDP port 14550 at Megabits/s bandwidth:
    - Full parameter downloads complete in 2 to 3 seconds.
    - Waypoint mission plans upload instantly in a fraction of a second.
    - Arm/Disarm and Guided commands execute with 0 ms RF latency.
    - Live FPV video stream (30 FPS MJPEG) runs concurrently on port 81.
  - *Tier 2 (Long-Range 433 MHz LoRa):* When the drone is flown beyond Wi-Fi range (beyond 50-80 meters), the operator connects QGC to the Ground Station ESP32 via Bluetooth Classic SPP or USB cable. Telemetry continues streaming reliably over LoRa for kilometers.
  - Both links are active simultaneously in the Air Unit firmware without interfering with each other.

### Issue 15: Battery Voltage Sags, RF Current Spikes & Silent ESP32 Brownout Drops
- **What happened:** During testing on a low battery, the ESP32-CAM Wi-Fi connection started dropping out repeatedly, forcing manual reconnection on the phone/laptop. Simultaneously, telemetry packets would freeze for 2 to 3 seconds before recovering.
- **The Root Cause:**
  - *Combined Peak RF Current Surges:* The ESP32 draws ~80 mA idle. When the Wi-Fi radio transmits frames (especially when QGC links or video frames are pushed), peak current surges to **250–350 mA**. The OV2640 camera sensor consumes **~160 mA** continuously. The LoRa Ra-02 transceiver draws **~100–120 mA** during transmission. Peak surge can exceed **600–700 mA**!
  - *Battery Internal Resistance & Rail Collapse:* When a 3S LiPo battery is depleted, its internal resistance spikes. Under heavy current draws, the battery voltage sags sharply. If the ESP32-CAM is powered from the flight controller's onboard 5V regulator (which is often rated for only 500 mA to 1A total and shared with the FC MCU and GPS), the 5V rail dips below 4.5V.
  - *Brownout Detector (BOD) Trip:* The AMS1117-3.3 LDO regulator on the ESP32-CAM requires at least 1.1V to 1.3V of headroom. If the 5V input drops to 4.2V, the 3.3V rail drops below 2.9V. This trips the ESP32 hardware Brownout Detector (`BOD`), causing an instant silent processor reboot. The reboot shuts down the Wi-Fi SoftAP, drops all client associations, wipes volatile serial buffers, and kills the telemetry link until the chip restarts 2 seconds later.
- **The Solution (APM Power Module Integration - CONFIRMED FLAWLESS):**
  - Instead of adding an external standalone regulator or rewiring the PDB, re-purposed the existing inline **APM Power Module** connected to the 3S LiPo battery.
  - The APM Power Module contains an onboard heavy-duty step-down switching regulator that outputs **+5.3V at up to 2.5A continuous current**.
  - Connected Pin 1 (+5.3V red wire) directly to the ESP32-CAM `5V` pin, and Pin 5/6 (GND) to the common ground.
  - Completely disconnected the +5V red wire from the F405 flight controller to prevent regulator back-feeding.
  - **Result:** The ESP32-CAM and LoRa module now receive isolated, high-current switching power. Wi-Fi streaming, high-speed MAVLink UDP, and LoRa telemetry now run rock-solid with zero brownouts or disconnects, even as battery voltage decreases.

### Issue 16: High-Throttle Voltage Sags Triggering Premature ArduPilot Failsafes
- **What happened:** During rapid throttle punchouts and aggressive flight maneuvers, four 1000KV motors with 1045 propellers pull 50A to 70A. The battery voltage naturally sags by 1.0V to 1.5V momentarily due to internal cell resistance. Because ArduPilot's battery monitor was reading raw voltage directly from the noisy PDB without an adequate confirmation delay, brief 1-second throttle bursts dropped the voltage below the low-voltage limit, immediately triggering an unwanted low-battery Land failsafe while the pack still had over 50% capacity!
- **The Root Cause:**
  - `BATT_LOW_TIMER` was set to default (0 or low seconds), causing instantaneous failsafe triggers on transient dips.
  - `BATT_LOW_VOLT` was set too high (e.g. 10.5V - 10.8V), leaving zero headroom for natural motor-induced voltage sags.
- **The Parameter Fix (Applied & Verified in ArduPilot):**
  - `BATT_LOW_TIMER = 10` (Introduced a 10-second confirmation window. ArduPilot now requires voltage to remain continuously low for 10 full seconds before executing a failsafe, completely filtering out transient 1-3 second throttle dips).
  - `BATT_LOW_VOLT = 10.2` (Configured realistic under-load threshold: 3.40V per cell under load; resting voltage returns to safe ~11.0V).
  - `BATT_CRT_VOLT = 9.9` (Set critical threshold: 3.30V per cell under load; resting voltage returns to ~10.5V).
  - `BATT_ARM_VOLT = 11.1` (Prevents arming on a discharged pack: 3.70V per cell).
  - `BATT_FS_LOW_ACT = 1` (Action on confirmed low battery: Land on the spot).
  - `BATT_FS_CRT_ACT = 1` (Action on critical battery: Land immediately).
- **Result:** Rapid throttle punchouts and high-current flight maneuvers no longer trigger false alarms or premature landings. The failsafe only activates when the battery is genuinely depleted.

### Issue 17: LoRa RF Link Starvation Caused by Parameter Streaming Lockout
- **What happened:** After deploying the ultra-fast MAVLink 2 passthrough and auto-baud firmware (which achieved sub-second Wi-Fi waypoint uploads), the FlySky Ground Station OLED unexpectedly displayed `LORA: NO RF SIGNAL` with zero packets received, despite the drone being powered and right on the bench.
- **The Root Cause:**
  1. *Parameter Flood Triggering False Mission Lockout:* In an attempt to prevent 40ms LoRa transmission cycles from delaying Wi-Fi waypoint handshakes, an activity timer (`isMissionActive`) was implemented to suppress LoRa transmissions during active transfers. However, `PARAM_VALUE` (Msg #22), `PARAM_REQUEST_LIST` (Msg #21), and `COMMAND_LONG` (Msg #76) were included in this trigger. Because ArduPilot streams over 900+ parameters upon boot and background sync, `lastMissionActivityTime` refreshed continuously.
  2. *Total Airtime Starvation:* With `isMissionActive` perpetually evaluated as `true`, `safeLoRaSend(stagingBuf, stagingLen)` was skipped on every iteration and `stagingLen` was reset to 0. Zero telemetry packets were transmitted over 433 MHz RF.
  3. *Diagnostic Ping Starvation:* The periodic 13-byte `$AG` heartbeat was guarded by `if (loraReady && stagingLen == 0)`. Because the F405 FC streams continuous UART telemetry, `stagingLen == 0` was never satisfied, silencing the heartbeat as well.
- **The Engineering Solution (Implemented in FW v2.5):**
  1. *Isolated Handshake Detection:* Removed `PARAM_VALUE`, `PARAM_REQUEST_LIST`, and general commands from mission lockout triggers. Restricted fast-flush triggers strictly to waypoint handshake messages (`MISSION_COUNT` #44, `MISSION_REQUEST_INT` #51, `MISSION_ITEM_INT` #73, `MISSION_ACK` #47, `MISSION_REQUEST` #40) and command confirmations (`COMMAND_ACK` #77).
  2. *Always-On Dual-Tier Transmission:* LoRa telemetry transmission is restored unconditionally alongside Wi-Fi UDP.
  3. *Zero-Latency UDP Service Loop in LoRa Driver:* Embedded a non-blocking `mavUdp.parsePacket()` service loop directly inside `safeLoRaSend`. While the SX1278 hardware transmits RF autonomously from its internal FIFO (taking 25-35 ms), any incoming UDP packet from QGC is parsed and forwarded to F405 FC UART within 1 ms, guaranteeing zero Wi-Fi delay.
  4. *Unconditional 1.2s RF Diagnostic Heartbeat:* The 13-byte `$AG` heartbeat frame transmits every 1.2 seconds unconditionally, ensuring Ground Station `airRfAlive` never expires even if the FC is unpowered or in DFU mode.
  5. *Ground Station Fast Frame Detection:* Upgraded Ground Station Bluetooth/USB uplink handling with instant MAVLink frame length detection. Once the exact packet length (e.g. 50 bytes for `MISSION_ITEM_INT`) is reached in the serial buffer, it dispatches over LoRa immediately without waiting for the 10-12 ms idle timeout.
- **Result:** Ground Station immediately locks RF signal (`LORA: OK (-65 dBm)` and live Fighter Jet HUD). Wi-Fi waypoint uploads remain sub-second, and LoRa waypoint uploads via Ground Station Bluetooth/USB operate at peak possible throughput.




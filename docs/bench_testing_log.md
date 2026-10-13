# My Bench Testing Diary

Here's my log of all the testing I've been doing on the bench. 

---

### Sept 25, 2026: Finally got the motors starting together!
Reflashed the board today to the proper `DAKEFPVF405` target. Before doing this, Motors 1 and 2 were starting way later than 3 and 4. I did a full ESC calibration sweep (2000 to 1000) using the Betaflight motor tab. Also got the FlySky receiver setup on the UART port. Now, all four motors start up beautifully at almost the exact same time (~1048µs). Big win for today!

### Testing the Radio Link
Plugged in the LiPo and tested the radio link. Everything is showing up perfectly in the receiver tab – no weird dropped frames or anything. It's super responsive.

### Setting up Switches
Mapped out my switches on the FlySky transmitter. I have AUX 1 set to arm the drone (safety first!), and I put ANGLE mode on the VRB knob so I can test self-leveling on the bench without it going crazy.

### Leveling the Drone
Just put it on my desk, leveled it out, and hit "Calibrate Accelerometer" in Betaflight. Looks perfectly flat on the 3D model now.

### Checking Motor Directions
I spun up each motor to make sure they were turning the right way for my quad-X setup. They all spin right, though Motor 4 felt a little stiff by hand. Might just be a tight screw, but I'll keep an eye on it. Oh, and I finally sketched out a quick diagram for the spin directions and saved it in the schematics folder so I don't forget.

### Tuning the Filters
The 500mm frame with big 10-inch props shakes quite a bit. I lowered the gyro and D-term filter multipliers to 0.8 just to smooth out the noise. This should help keep the ESCs from getting super hot while it tries to correct tiny vibrations.

### Tweaking the PIDs for this beast
These big props and old analog ESCs needed some custom PID tuning. I completely turned off Feedforward so it doesn't twitch on the bench. I also cut the P and D gains in half to stop it from over-correcting, and bumped the I-gain up to compensate. Seems a lot more stable for testing now!

### The Tether Setup
Getting ready for live power testing. Tied the drone down to a massive 15kg dumbbell. I am NOT letting this thing fly across the room again. 

### Investigating Weird Noises
I noticed the motors were creaking a lot at idle. Realized it was the low PWM frequency making the stators vibrate. Also figured out why Motor 4 felt weird earlier – when it's powered, the ESC basically locks up because of a shorted MOSFET acting like a brake. Super annoying, but at least I know what's broken now!

### The Crash
Yeah... I tested it untethered before figuring out the Motor 4 issue. Spun it up a bit, the PID loop went nuts trying to level it, and when I killed the throttle, Motor 4 braked instantly while the others kept spinning. Flipped right into a wall. Two props dead, one cracked. I'm taking a break until my new parts arrive!

### Sept 25, 2026: ESC Repair & Pre-Tuning Setup
**The Repair:**
Successfully swapped out the dead ESC for Motor 4. Instead of re-wiring the whole arm, I just desoldered the power/signal wires from the old board and soldered them straight to the new one.

**Results:**
- Motor 4 powers up beautifully.
- The weird electromagnetic braking (shorted MOSFET) is completely gone.
- Spun it up briefly and confirmed it's rotating in the right direction!

**Next Steps:**
Going to recalibrate all four ESCs together to ensure perfect synchronization, and then it's time to strap it to the anchor and get into the PID tuning!

---

### Sept 25, 2026: ESC Calibration & Anchor Prep
**Calibration Success:**
- Recalibrated all four ESCs simultaneously. The synchronization is spot on!
- Set the motor idle up to **7.0%** to ensure reliable, stutter-free startup and smooth commutation across the legacy hardware.

**Next Phase:**
Preparing the 15kg dumbbell anchor setup. It's time to run tethered live-power tuning tests to dial in the PIDs!

---

### Sept 26, 2026: Anchor Evolution for Hover Testing
I've been working on a safe anchor setup using nylon ropes to prevent the drone from flying away or flipping during tuning. Here is how it evolved:

**Step 1: The 15kg Dumbbell**
- I started with a single 15kg dumbbell.
- *Problem:* The dumbbell was too big, creating a high risk of the 10-inch propellers crashing into the weight.
- ![Anchor Step 1](../assets/testbed/anchor_step_1_15kg.jpeg)

**Step 2: Dual 5kg Dumbbells**
- I switched to two 5kg dumbbells, tied to both sides of the drone.
- *Problem:* While better, there was still a slight possibility of the propellers striking them during a hard tilt.
- ![Anchor Step 2](../assets/testbed/anchor_step_2_5kg_split.jpeg)

**Step 3: The Initial Suitcase Platform**
- I placed a suitcase directly under the drone and placed the dumbbells *outside* alongside it. 
- *Result:* This totally eliminated the risk of propeller strikes! When the drone lifts, the anchor allows for a little more than 7 inches of vertical lift. This is the perfect "goldilocks" zone—high enough to test hovering, but low enough to prevent major instability or bad tuning issues from causing a crash.
- ![Anchor Step 3](../assets/testbed/anchor_step_3_suitcase.jpeg)

**Step 4: Bigger Suitcase & Quick-Release Hooks**
- *The Platform:* Switching to a bigger suitcase because the previous one wasn't wide enough for all four arm corners to rest completely flat.
- *Rope Adjustments:* Because this new suitcase is taller/higher, I am increasing the rope length slightly to maintain the optimal vertical travel window.
- *Quick Attachment:* Integrating removable hooks on the tether lines instead of manually tying and untying knots every time—now I can just hook and unhook the corners in seconds.

---

### Sept 26, 2026: Failsafe Testing (Signal Loss)
**Objective:** Test what happens if the transmitter loses connection or runs out of battery.
- **The Test:** Turned on the drone, spooled up the motors slightly, and then completely turned off the transmitter.
- **The Problem:** The motors just kept spinning! The receiver wasn't telling the flight controller to cut the throttle.
- **The Fix:** Configured the failsafe directly on the transmitter. Set Channel 3 (Throttle) to drop to -100%, and Channel 5 (Arming switch) to the disarm position upon signal loss.
- **Result:** Retested. Exactly 1.5 seconds after turning off the remote, the motors completely shut down, perfectly matching the Betaflight failsafe delay configuration. Ready for safe hover testing!

---

### Oct 03, 2026: Live Test 01 – Hardware & Arming Check (Strapped)
**Objective:** Live power check with full props on to verify arming, motor synchronization, and commutation following the Motor 4 ESC replacement.
- **Setup:** Quad strapped down flat to the suitcase anchor testbed to eliminate any flip/crash hazard.
- **Results:**
  - All four motors armed synchronously without stutter or desync.
  - Motor 4 ran completely clean under load with no drag or excessive heat, confirming the ESC fix.
  - Throttle and directional stick responses were verified.
  - **Tuning Observation:** Running the stock Betaflight 5" miniquad PID preset resulted in aggressive motor hunting and rapid RPM oscillation against the frame inertia. Confirmed need to flash custom low-gain 500mm PIDs before attempting tethered hover.
- **Detailed Log & Video:** See the full report and footage in [Test 01 Log](testing_logs/test_01/README.md).

---

### Oct 03, 2026: Live Test 02 – Untethered Takeoff Attempt (Custom PIDs)
**Objective:** Test liftoff and hover stability after reducing PID gains from stock miniquad defaults.
- **Setup:** Untethered on the floor.
- **Results:**
  - Arming and initial spin-up were smooth.
  - As soon as the quad gained enough throttle to break ground contact, it entered an extreme, rapid wobble across both roll and pitch.
  - The violent oscillation caused the quad to pitch hard and tip over onto its arm.
  - Promptly cut throttle and disarmed for safety. No catastrophic damage, but clearly shows analog ESC response lag and over-correction against 10-inch prop inertia.
  - **Decision:** Moving all future hover tests back to the Step 4 tethered suitcase rig, and dropping gains further.
- **Detailed Log & Video:** See the full report and video in [Test 02 Log](testing_logs/test_02/README.md).

---

### Oct 03, 2026: Off-Camera Bench Tuning & M10Q GPS Arrival
After seeing that nasty wobble during Test 02's liftoff attempt, I spent a solid chunk of time on the bench doing a bunch of precise, off-camera tunings to get things under control before risking another live run:
- **Smoothing out the loop:** Refined the PID gains further and tweaked the filter sliders to prevent motor noise from feeding back into the analog ESCs. The goal was to eliminate the phase lag between the flight controller and the heavy 10-inch props.
- **Pre-Test Check:** Ran bench commutation and individual motor sweeps without props to confirm signals stayed crisp and free of jitter across the throttle curve.
- **Hardware Upgrade (M10Q 250 GPS + 5883 Compass Arrived!):** My new M10Q GPS with the integrated QMC5883L compass just arrived in the mail. The game plan now:
  1. Run Test 03 to verify these off-camera fine-tunings.
  2. Wire up the GPS (UART) and compass (I2C) to the DakeFPV F405 and test the sensor feeds in Betaflight.
  3. Once everything is confirmed stable, make the official switch over to ArduPilot for autonomous flight and mission planning.

---

### Oct 04, 2026: Live Test 03 – Stable Hover Achieved (Wobble Eliminated!)
**Objective:** Test liftoff and hover stability after off-camera PID refinements, zeroing D-Max, and heavily downgrading Angle mode strength.
- **Setup:** Untethered on floor.
- **Results:**
  - Liftoff was super smooth and controlled.
  - The violent wobble from Test 02 was completely eliminated! The drone hovered steadily at knee and waist height across multiple touch-and-go cycles.
  - Used the VRB knob to test Angle mode self-leveling in real time with zero snapping or oscillation.
  - Slight positional drift occurred as expected since there is no GPS or optical flow sensor yet.
- **Detailed Log & Video:** See the full writeup and footage in [Test 03 Log](testing_logs/test_03/README.md).

---

### Oct 04, 2026: Live Test 04 – In-Flight Failsafe Drop Test
**Objective:** Verify that the flight controller immediately kills motor power in mid-air upon signal loss / failsafe trigger.
- **Setup:** Hovering in the room at approximately eye level.
- **Results:**
  - Drone hovered rock-solid in place.
  - Triggered failsafe: at roughly the 0:36 mark, all four motors instantly died and the quad dropped flat to the floor.
  - Zero motor run-on, no throttle hang, and zero flyaway hazard. Failsafe is 100% verified under real flight load.
- **Detailed Log & Video:** See the full writeup and footage in [Test 04 Log](testing_logs/test_04/README.md).

---

### Oct 04, 2026: Mount Builds & ArduPilot Hex Verification
- **Mounts Completed:**
  - Built the folding GPS mast mount to elevate the M10Q GPS and keep the magnetometer away from the PDB's magnetic field.
  - Built the vibration isolation mount for the DakeFPV F405 using a squarely cut credit card plate backed with double-sided foam tape to absorb motor vibrations.
- **ArduPilot 1MB Flash Check:**
  - Tested the stock hex binary in `firmware/arducopter_with_bl.hex` (ArduCopter V4.7.1 with bootloader for DAKEFPVF405).
  - Memory span: `0x08000000` to `0x080DDA60` = 886.59 KB (907,872 bytes).
  - Result: FITS cleanly inside the 1MB (1024 KB) flash with 137.41 KB of free headroom. It is not overloaded, so we can flash this stock build directly!

---

### Oct 04, 2026: ArduPilot Flashed via STM32CubeProgrammer
- **Flashing Headaches:** Tried flashing the local `arducopter_with_bl.hex` through Betaflight Configurator, but it kept failing/refusing to write the image. Betaflight's flasher expects its own partition layout and doesn't like flashing foreign bootloaders.
- **The Solution:** Fired up **STM32CubeProgrammer**, put the F405 into DFU mode, and flashed the hex directly to the chip with a full chip erase.
- **Status: Flashed successfully!** The board is officially running ArduPilot (ArduCopter V4.7.1). Now ready to fire up Mission Planner and start setting up the frame and sensors.

---

### Oct 04, 2026: GPS Success & Compass SCL Diagnostics (SOLVED!)
- **GPS Status:** Connected the M10Q module to the UART port. GPS is working great in ArduPilot and acquiring satellite fixes.
- **Compass Issue & Multimeter Diagnostics:**
  - The onboard compass wasn't showing up in ArduPilot despite tweaking all compass parameters.
  - Checked resistance across lines: readings were completely normal, confirming no dead short to ground.
  - Unpowered check: With the GPS plugged into the FC but unpowered, both SDA and SCL floated around ~1.5V.
  - Powered check: Once 5V was supplied to the GPS, SDA pulled up to 3.3V, but SCL remained stuck down at 0.2V.
  - The Isolation Test: Desoldered the SCL wire from the FC pad. The loose wire from the GPS immediately measured **3.3V**, and the FC pad showed no short to ground. Hardware and wiring were completely fine!
- **The Breakthrough (Software Bus Conflict):**
  - Traced the issue to an ArduPilot parameter conflict: `NTF_LED_TYPES` (or `LED_TYPE`) was set to `455` (enabling external I2C LED drivers).
  - Because no external I2C LED was attached, the driver grabbed and locked up the I2C bus on boot, dragging the SCL line down to 0.2V and preventing compass communication.
  - **The Fix:** Changed the parameter to **1** (internal board LEDs only). Rebooted, SCL instantly jumped to 3.3V, and the compass is now 100% detected and functional in Mission Planner!

---

### Oct 05, 2026: The 8-Hour ESC Boot Timing Mystery (ArduPilot vs Legacy ESCs)
- **The Issue:** Plugging in the LiPo directly resulted in completely silent, dead ESCs that refused to arm or produce startup chimes. Yet, whenever the flight controller was pre-powered via USB, plugging in the battery made all four ESCs arm and run perfectly.
- **The 8-Hour Nonstop Grind:** Spent 8 hours nonstop testing every parameter permutation in Mission Planner, re-checking wiring connections, and troubleshooting solder points trying to isolate why battery-only power failed.
- **The Mobile Charger Breakthrough:** Tested a boot timing theory by using a phone charger to power up the FC's 5V input first. Allowed ArduPilot a few seconds to complete its boot sequence and sensor init, then plugged in the LiPo. All four ESCs immediately sang their startup tones and initialized flawlessly!
- **The Root Cause:** Legacy analog ESCs feature an internal hardware timeout. If they do not detect a valid zero-throttle PWM signal within a couple seconds of receiving power, they lock out to prevent runaways. Betaflight boots in milliseconds, easily beating this timer. ArduPilot takes several seconds to boot its ChibiOS RTOS, EKF, and safety checks before enabling PWM outputs, causing the ESCs to time out when both are powered simultaneously from the LiPo.
- **Operational Solution (The Phone Startup Trick):** Instead of cutting wires or adding physical switches just to accommodate these frustrating legacy ESCs, I found a way better field solution:
  - Plug a standard USB Type-C to Type-C cable from my mobile phone into the FC's USB port.
  - The phone boots the F405 via reverse charging. Wait a few seconds for ArduPilot to finish its startup sequence and start driving PWM pulses.
  - Plug in the main 3S LiPo battery—the ESCs hear the signal immediately and initialize cleanly.
  - Unplug the cable from the phone/FC (the FC stays powered from the LiPo) and we are good to fly!
  - Zero extra hardware or messy wiring modifications needed until I can save up for modern BLHeli_S/32 ESCs down the road.

---

### Oct 05, 2026: ArduPilot Failsafe Recalibration & AutoTune Flight Prep
- **Failsafe Recalibration:** Completed full recalibration of all failsafe mechanisms in Mission Planner (Radio Failsafe on signal loss, Battery Failsafe low-voltage triggers, and GCS failsafe).
- **Pre-Flight Status:**
  - Sensor calibration verified (accelerometer, M10Q GPS lock, and QMC5883L compass).
  - Ready for initial manual takeoff and basic hover verification.
  - AutoTune mode mapped to an auxiliary switch to automatically calculate optimal rate PIDs for the 500mm frame once airborne in a stable AltHold hover.

---

### Oct 05, 2026: Motor Mapping Fix & 5-Mode Transmitter Mixer Setup
- **Motor Ordering & Directions:** Corrected motor mapping and rotational directions to match ArduCopter Quad-X standards (aligning with ArduPilot's motor numbering and spin directions, which differ from Betaflight).
- **Clever 5-Flight-Mode Radio Mixing (FlySky FS-i6):**
  - Standard radios make it tough to get more than 3 flight modes out of a single switch, but on the 6-channel FlySky FS-i6 I set up a programmable mixer mixing the 2-position switch (Channel 6) into the 3-position switch (Channel 5).
  - By shifting the output PWM bands, this gives access to 5 distinct flight modes across two switch banks:
    - **Bank 1 (2-Pos Switch Off):**
      - Position 1: **Stabilize** (manual self-leveling)
      - Position 2: **AltHold** (barometer altitude hold)
      - Position 3: **Loiter** (full GPS position and altitude hold)
    - **Bank 2 (2-Pos Switch On):**
      - Position 1: **Stabilize** (always available as a manual bailout)
      - Position 2: **RTL** (Return-To-Launch autonomous return)
      - Position 3: **AutoTune** (automated in-flight PID tuning)

---

### Oct 05, 2026: Off-Camera Maiden Flight & Aggressive Maneuver Crash
- **Flight Testing (Off-Camera):** Took the quad out for its maiden ArduPilot flight tests. Didn't have a camera set up for recording, so this was run completely off-camera.
- **The Crash:** Pushed the quad hard into some aggressive maneuvers to test attitude recovery and limits, resulting in a crash during one of the aggressive test passes.
- **Outcome & Airworthiness:** 
  - Quad was recovered safely with no critical hardware loss.
  - Core flight stability is confirmed: the quad is remarkably stable now in the air under ArduPilot.

---

### Oct 06, 2026: ArduPilot Flight Verification Phase (Tests 05, 06, and 07)
With the initial off-camera shakeouts and transmitter mixer setup behind us, I set up the whiteboard and ran full video-documented verification of the ArduPilot firmware stack across three core milestone tests:

1. **Test 05 (Arm Test):**
   - Goal: Achieve Successful Flight (Arm Check).
   - Flipped the arm switch on the FlySky FS-i6; all four 2212 motors spun up cleanly at idle without stator chatter, hesitation, or desync. Small throttle blips confirmed synchronous throttle response, and disarming instantly killed power.
   - Outcome: Passed with flying colors. Full log: [`docs/testing_logs/test_05/README.md`](file:///d:/MIT/Projects/Drone/quad-commutation-testbed/docs/testing_logs/test_05/README.md).

2. **Test 06 (Stabilization Test):**
   - Goal: Achieve Successful Flight (Manual Stabilization).
   - Brought the quad up into ground effect and a 1-meter hover in Stabilize mode.
   - The quad self-leveled crisply with zero trace of the violent wobble seen in early Betaflight tests. Cyclic stick commands (roll/pitch/yaw) were predictable, with smooth attitude recovery and a gentle landing.
   - Outcome: Passed with flying colors. Full log: [`docs/testing_logs/test_06/README.md`](file:///d:/MIT/Projects/Drone/quad-commutation-testbed/docs/testing_logs/test_06/README.md).

3. **Test 07 (Alt-Hold Test):**
   - Goal: Achieve Successful Flight (Automated Altitude Hold).
   - Lifted off in Stabilize, switched to Alt-Hold (Bank 1, Position 2), and centered the throttle stick.
   - The barometer and EKF z-axis estimator locked altitude rock-solid in the middle of the room ~1.5 meters up, ignoring indoor ground effect and turbulence. Cyclic corrections were clean while altitude stayed locked automatically.
   - Outcome: Passed with flying colors. Full log: [`docs/testing_logs/test_07/README.md`](file:///d:/MIT/Projects/Drone/quad-commutation-testbed/docs/testing_logs/test_07/README.md).

- **Current Status & Transition Outdoors:**
  - Indoor flight envelope expansion on ArduPilot was 100% complete and proven rock-solid.
  - Moved outdoors to an open field to test GPS navigation and autonomous Return-To-Launch (RTL).

---

### Oct 06, 2026: Outdoor RTL Verification, Tree Snag, & The Barehanded Rescue Catch
Following the successful indoor tests, took the quad outside to test autonomous GPS navigation:

1. **Test 08 (Outdoor Return-To-Launch Verification):**
   - Goal: Autonomous RTL navigation via GPS & compass.
   - Tested RTL twice in the open field. The quad climbed autonomously to 15m (`RTL_ALT = 1500`), rotated towards Home, flew across the field, and positioned itself right over the exact takeoff point. Flawless autonomous performance. Full log: [`docs/testing_logs/test_08/README.md`](file:///d:/MIT/Projects/Drone/quad-commutation-testbed/docs/testing_logs/test_08/README.md).

2. **The Low-Battery Failsafe Tree Incident:**
   - Walking the quad back home with the remaining charge, the 3S 2200mAh pack reached low-voltage failsafe (`BATT_FS_LOW_ACT = 2` / RTL).
   - ArduPilot instantly triggered autonomous RTL back to the original field launch coordinates, climbing to 15m.
   - En route, it encountered the tallest tree in the neighborhood (~18-20m tall) and wedged itself firmly in the top branches.

3. **The Multi-Stage Rescue Operation:**
   - Tree climb attempt: climbed up, but the top branches where the drone hung were too narrow and fragile to bear weight. A dead tree branch used as a poker also fell short.
   - Fire brigade call: declined because their protocol only permits response to human life-or-death emergencies.
   - Custom high-reach tool: spliced an ultra-long bamboo pole with a lightweight PVC pipe, reinforced by a wooden batten lashed along the joint.
   - The drop & diving catch: helpers dislodged the drone with the pole while I held the safety cloth below; the drone took an unexpected outward trajectory away from the cloth, prompting a split-second sprint and barehanded diving catch!
   - Damage: miraculously, only 1 propeller broke. Frame, carbon arms, motors, ESCs, FC, GPS mast, and all sensors survived 100% unharmed. Detailed post-mortem logged in [`docs/failure_logs.md`](file:///d:/MIT/Projects/Drone/quad-commutation-testbed/docs/failure_logs.md) under Issue 11.

4. **Failsafe Parameter Configuration:**
   - **Low-Battery Failsafe Set to Land (`BATT_FS_LOW_ACT = 1`):** Completely eliminates the risk of autonomous cross-neighborhood transits or climbs when battery cells are depleted. The quad will descend and land vertically in place on low voltage.
   - **RTL Altitude Raised to 22m (`RTL_ALT = 2200`):** For deliberate, switch-commanded RTL returns, the cruising altitude was increased to 22 meters (2200cm) to guarantee total clearance over the tallest neighborhood tree canopy (~18-20m), permanently resolving the tree snag hazard.
   - Replaced broken 1045 prop; battery recharged and all sensor telemetry verified healthy in Mission Planner. Ready for next flight phase.







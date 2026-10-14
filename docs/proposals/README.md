# Engineering Proposals & Architecture RFCs

This directory contains formal Requests for Comments (RFCs), hardware proposals, and architectural blueprints for the 500mm quadcopter commutation and autonomy testbed.

---

## Proposal Lifecycle

Every idea follows a structured lifecycle to ensure hardware safety, electrical compatibility, and performance verification before a single wire is soldered or firmware code is changed:

```
[ IDEA ] ---> [ UNDER REVIEW ] ---> [ APPROVED ] ---> [ IN PROGRESS ] ---> [ IMPLEMENTED ]
                      |
                      +------------> [ REJECTED / DEFERRED ]
```

| Status | Definition |
| :--- | :--- |
| **`IDEA`** | Concept drafted, open for discussion, brainstorming, and feasibility assessment. |
| **`UNDER REVIEW`** | Technical specs, power budgets, pinouts, and trade-offs under evaluation. |
| **`APPROVED`** | Cleared by the pilot/lead engineer. Ready for hardware procurement or prototyping. |
| **`IN PROGRESS`** | Active bench testing, wiring, or software development underway. |
| **`IMPLEMENTED`** | Verified in flight, documented in failure and testing logs, and merged into main codebase. |
| **`DEFERRED`** | On hold due to budget, weight, or technical constraints. |

---

## Proposals Index

| ID | Title | Category | Target Hardware | Status | Document |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **RFC-001** | Raspberry Pi Companion Computer & Edge AI Engine | Autonomy / Vision | Pi Zero 2W / Pi 4B + Camera | **`APPROVED`** | [RFC-001 Document](0001_companion_computer_vision.md) |
| **RFC-002** | DroneBridge & ESP-NOW vs. 433 MHz LoRa Evaluation | RF / Telemetry | ESP32-CAM + Ra-02 | **`DEFERRED`** | [RFC-002 Document](0002_esp_now_vs_lora_analysis.md) |
| **RFC-003** | Advanced Autonomy & Sensor Fusion Roadmap | Expansion / Sensors | LiDAR, Optical Flow, 4G LTE | **`IDEA`** | [RFC-003 Document](0003_advanced_autonomy_roadmap.md) |

---

## How to Propose a New Feature
1. Create a new markdown file in this directory following the naming pattern: `000X_<short_slug>.md`.
2. Include the standard sections:
   - **Executive Summary & Motivation**
   - **Hardware Architecture & Wiring Blueprint**
   - **Power Budget & Thermal Impact**
   - **MAVLink / Software Data Flow**
   - **Risk Analysis & Mitigations**
   - **Implementation Milestones**
3. Add the entry to the index table above with status `IDEA`.

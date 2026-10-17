# ESP32 Matter CW-WW (CCT) LED Strip Controller

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Matter](https://img.shields.io/badge/Matter-1.x-blue.svg)](https://csa-iot.org/all-solutions/matter/)
[![Target: ESP32-C3 / ESP32-C6](https://img.shields.io/badge/Target-ESP32--C3%20%7C%20C6-red.svg)](https://www.espressif.com/en/products/socs)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.1%2B-green.svg)](https://github.com/espressif/esp-idf)
[![Framework: ESP-Matter](https://img.shields.io/badge/Framework-esp--matter-orange.svg)](https://github.com/espressif/esp-matter)

Production-grade Espressif Matter firmware for a Dual-Channel Tunable White (Cool White / Warm White) LED controller built on the **ESP32-C3** (default) or **ESP32-C6**, running **Matter over Wi-Fi** with Bluetooth Low Energy (BLE) commissioning.

Compatible natively with **Apple Home**, **Google Home**, **Home Assistant**, and **Samsung SmartThings**.

---

## Architecture Overview

```text
┌────────────────────────────────────────────────────────────────────┐
│               Smart Home Fabrics (Matter Ecosystems)               │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │
│  │    Apple Home    │  │   Google Home    │  │  Home Assistant  │  │
│  │ (iOS / HomePod)  │  │(Android / Google)│  │ (Matter Native)  │  │
│  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘  │
└───────────┼─────────────────────┼─────────────────────┼────────────┘
            │                     │                     │
            │ BLE / Wi-Fi         │ Wi-Fi (IPv6)        │ Wi-Fi (IPv6)
            ▼                     ▼                     ▼
┌────────────────────────────────────────────────────────────────────┐
│                 Wireless Transport & Commissioning                 │
│  ┌───────────────────────────┐  ┌───────────────────────────────┐  │
│  │ Bluetooth LE Commissioning│  │  2.4 GHz Wi-Fi (Matter)       │  │
│  │ - Fast BLE Advertising    │  │  - Thread-free Wi-Fi Engine   │  │
│  │ - Secure Wi-Fi Provision  │  │  - Multi-Admin UDP / IPv6     │  │
│  └─────────────┬─────────────┘  └───────────────┬───────────────┘  │
└────────────────┼────────────────────────────────┼──────────────────┘
                 │                                │
                 │ BLE Provisioning Stream        │ Matter Cluster Commands
                 ▼                                ▼
┌────────────────────────────────────────────────────────────────────┐
│              ESP32-C3 / ESP32-C6 Matter Controller                 │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │            ESP-Matter Core Engine (Thread-Safe)              │  │
│  │  - Endpoint 1: Color Temperature Light (0x010C)              │  │
│  │  - Clusters: OnOff (0x0006), Level (0x0008), Color (0x0300)  │  │
│  │  - Non-Volatile Storage (NVS): Boot State Restoration        │  │
│  └──────┬───────────────────────┬──────────────────────┬────────┘  │
│         │                       │                      │           │
│         ▼                       ▼                      ▼           │
│  ┌──────────────┐      ┌─────────────────┐    ┌─────────────────┐  │
│  │ Status Timer │      │ Button ISR/Task │    │ LEDC HW Engine  │  │
│  │ (esp_timer)  │      │(GPIO 9 Wakeup)  │    │ (4kHz, 13-Bit)  │  │
│  └──────┬───────┘      └────────▲────────┘    └───────┬───┬─────┘  │
└─────────┼───────────────────────┼─────────────────────┼───┼────────┘
          │                       │                     │   │
          │ GPIO 8 (Active-Low)   │ GPIO 9 (Input)      │   │
          ▼                       │                     │   │
┌───────────────────┐    ┌────────┴────────┐            │   │
│    Status LED     │    │   Push Button   │            │   │
│  - Fast: Pairing  │    │   (BOOT / SW1)  │            │   │
│  - Off: Paired    │    │ - Short: Toggle │            │   │
│  - Slow: Identify │    │ - 10s: Reset    │            │   │
└───────────────────┘    └─────────────────┘            │   │
                                                        │   │
                ┌───────────────────────────────────────┘   │
                │ GPIO 0 PWM (CW)           ┌───────────────┘
                │                           │ GPIO 1 PWM (WW)
                ▼                           ▼
  ┌───────────────────────────┐       ┌───────────────────────────┐
  │  N-MOSFET: Cool White CW  │       │  N-MOSFET: Warm White WW  │
  │  - Logic-Level (AO3400A)  │       │  - Logic-Level (AO3400A)  │
  │  - 100 Ω Gate / 10 kΩ PD  │       │  - 100 Ω Gate / 10 kΩ PD  │
  │  - Reverse Schottky Diode │       │  - Reverse Schottky Diode │
  └─────────────┬─────────────┘       └─────────────┬─────────────┘
                │                                   │
                │ Cool White PWM                    │ Warm White PWM
                ▼                                   ▼
┌────────────────────────────────────────────────────────────────────┐
│       Dual-Channel Tunable White (CCT) LED Strip (12V / 24V)       │
│      - Cool White (CW): 6500K            - Warm White (WW): 2700K  │
└────────────────────────────────────────────────────────────────────┘
```

---

## 1. Hardware Pinout & Circuit Guidelines

| Function | Pin | Signal Type | Driver Circuit | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Cool White (CW)** | `GPIO 0` | Active-High PWM | N-Channel MOSFET Gate | 4 kHz, 13-bit resolution (`8191` steps) |
| **Warm White (WW)** | `GPIO 1` | Active-High PWM | N-Channel MOSFET Gate | 4 kHz, 13-bit resolution (`8191` steps) |
| **Status LED** | `GPIO 8` | Active-Low Output | Current-limiting resistor (e.g. 1kΩ) | **Strapping Pin!** Keep Active-Low to 3.3V. |
| **Control Button** | `GPIO 9` | Active-Low Input | Internal pull-up to 3.3V | **Strapping Pin (BOOT)!** Keep external filter cap $\le 10\text{ nF}$. |

### Recommended Electrical Design & MOSFET Selection

#### 1. Logic-Level MOSFETs at 3.3V Gate Drive
* Standard power MOSFETs (like the older `IRLZ44N` or `FQP30N06L`) have high gate capacitance ($C_{iss} \approx 3300\text{ pF}$) and require $V_{GS} \ge 4.5\text{–}10\text{ V}$ to achieve full saturation. Direct 3.3V GPIO drive through a $100\,\Omega$ resistor causes significant switching heat and pulse attenuation.
* **Recommended Direct-Drive MOSFETs ($V_{GS} = 2.5\text{ V}$ rated, low $Q_g$):**
  * **AO3400A** (SOT-23, 30V / 5.7A, $R_{DS(on)} < 33\,\text{m}\Omega$ @ 2.5V) — ideal for strips up to 3A per channel.
  * **IRLML6344TRPBF** (SOT-23, 30V / 5A) or **PMV16XN** (SOT-23, 30V / 7A).
  * **AON7400A** (DFN 3x3, 30V / 30A, $R_{DS(on)} < 2.5\,\text{m}\Omega$) — for high-power installations ($>5\text{A}$).
* If driving high-capacitance TO-220 MOSFETs or running $>10\text{ A}$, use a dedicated dual gate driver IC (e.g. **TC4427**, **MIC4427**, or **UCC27524**) powered from 12V.

#### 2. Inductive Flyback Protection (Schottky Diodes)
Long 12V/24V LED strip cables have parasitic inductance ($1\text{–}3\,\mu\text{H}$). Rapid PWM turn-off induces high voltage inductive spikes ($V = L \frac{\mathrm{d}I}{\mathrm{d}t}$) at the MOSFET drain.
* Install a fast **Schottky diode** (e.g. **SS34** / **1N5822**, 3A, 40V) across each LED channel:
  * **Cathode** $\rightarrow$ Strip $+12\text{V} / +24\text{V}$ rail
  * **Anode** $\rightarrow$ MOSFET Drain (`CW-` and `WW-`)

#### 3. Critical Strapping Pin Rules (ESP32-C3 & ESP32-C6)
* **GPIO 8**: Determines boot mode on reset. Must be **HIGH** during reset to allow entering serial download boot mode (`GPIO 9 = LOW`). Wire the Status LED as **Active-Low** (LED cathode to GPIO 8, anode via resistor to 3.3V) so GPIO 8 is not pulled down to GND at boot.
* **GPIO 9**: BOOT pin. Must be **HIGH** for normal SPI flash boot. If adding an external debounce capacitor to the button, keep it $\le 10\text{ nF}$; larger capacitors hold GPIO 9 LOW during power-up reset, trapping the ESP32 in download mode.

#### 4. Power Supply Decoupling
* Place a $220\,\mu\text{F}\text{–}470\,\mu\text{F}$ low-ESR electrolytic capacitor + a $10\,\mu\text{F}$ ceramic MLCC directly across the 12V/24V input near the MOSFET sources.

---

## 2. Key Features

* **Matter Device Type**: `Color Temperature Light` (`0x010C`):
  * **OnOff Cluster**: Power control with non-volatile state restoration after power cycles.
  * **LevelControl Cluster**: Brightness control (1–254) with smooth **$\gamma = 2.2$ gamma correction**.
  * **ColorControl Cluster**: Full **50 to 400 Mired** range unlocking the complete **Light Blue $\rightarrow$ White $\rightarrow$ Orange** Apple Home spectrum mapped $0\text{–}100\%$ across CW/WW.
* **Ratio-Locked 200 Hz Smooth Fade**: Eliminates turn-on/turn-off color shifts by locking the CW/WW chromaticity ratio while ramping luminance.
* **Always-On Wi-Fi Robustness**: Disables Wi-Fi modem sleep (`WIFI_PS_NONE`), enforces `HT20` 20 MHz bandwidth and `20 dBm` max TX power, and runs a 60s mDNS/reconnect watchdog for zero *"No Response"* dropouts in Apple Home.
* **Zero Low-End Dead Zone**: Guarantees visible illumination with accurate CW/WW balance even at 1% brightness.
* **Thread-Safe Architecture**: Physical button events safely scheduled onto the Matter event thread.
* **Low Idle CPU Overhead**: Interrupt-driven button handling and `esp_timer`-driven status indication.
* **Custom Static MAC Address**: Configurable via Kconfig or runtime NVS override (`custom_cfg` / `custom_mac`).
* **Dual OTA Partitions & Rollback**: Automatic bootloader rollback protects against bricked OTA updates.

---

## 3. How to Build & Flash

### Prerequisites
* ESP-IDF **v5.1.x** or **v5.2.x** installed.
* Espressif Matter Framework (`esp-matter`).

### 1. Build Optimization (Recommended on Windows)
```powershell
.\optimize-build.ps1
```

### 2. Set Target (Default: ESP32-C3)
```bash
idf.py set-target esp32c3
```
*(To build for ESP32-C6, run `idf.py set-target esp32c6`).*

### 3. Configure (Optional)
```bash
idf.py menuconfig
```
Navigate to:
> `CW-WW LED Controller Configuration`

### 4. Build, Flash & Monitor
```bash
idf.py build
idf.py -p COMx flash monitor
```

---

## 4. Pairing & Operation

1. Power on the controller. The status LED (`GPIO 8`) will blink quickly (pairing mode).
2. The serial console prints the QR code URL and the **11-digit Manual Pairing Code**.
3. Open **Apple Home**, **Google Home**, or **Home Assistant** $\rightarrow$ **Add Accessory** $\rightarrow$ Scan or type pairing code.
4. **Physical Controls**:
   * **Short Press (GPIO 9)**: Toggle On / Off.
   * **10s Long Press (GPIO 9)**: Clear Matter fabrics and trigger factory reset.

---

## License

This project is licensed under the [MIT License](LICENSE).

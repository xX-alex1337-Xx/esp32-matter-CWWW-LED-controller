# ESP32-C6 Matter CW-WW (CCT) LED Strip Controller

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Matter](https://img.shields.io/badge/Matter-1.x-blue.svg)](https://csa-iot.org/all-solutions/matter/)
[![Target: ESP32-C6](https://img.shields.io/badge/ESP32--C6-RISC--V-red.svg)](https://www.espressif.com/en/products/socs/esp32-c6)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.1%2B-green.svg)](https://github.com/espressif/esp-idf)
[![Framework: ESP-Matter](https://img.shields.io/badge/Framework-esp--matter-orange.svg)](https://github.com/espressif/esp-matter)

Official Espressif Matter firmware for a Dual-Channel Tunable White (Cool White / Warm White) LED controller built on the **ESP32-C6**, running **Matter over Wi-Fi** (2.4 GHz Wi-Fi 6) with Bluetooth Low Energy (BLE) commissioning.

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
            │ BLE / Wi-Fi 6       │ Wi-Fi 6 (IPv6)      │ Wi-Fi 6 (IPv6)
            ▼                     ▼                     ▼
┌────────────────────────────────────────────────────────────────────┐
│                 Wireless Transport & Commissioning                 │
│  ┌───────────────────────────┐  ┌───────────────────────────────┐  │
│  │ Bluetooth LE Commissioning│  │  2.4 GHz Wi-Fi 6 (Matter)     │  │
│  │ - Fast BLE Advertising    │  │  - Thread-free Wi-Fi 6 Engine │  │
│  │ - Secure Wi-Fi Provision  │  │  - Multi-Admin UDP / IPv6     │  │
│  └─────────────┬─────────────┘  └───────────────┬───────────────┘  │
└────────────────┼────────────────────────────────┼──────────────────┘
                 │                                │
                 │ BLE Provisioning Stream        │ Matter Cluster Commands
                 ▼                                ▼
┌────────────────────────────────────────────────────────────────────┐
│                     ESP32-C6 Matter Controller                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │            ESP32-C6 SoC (ESP-Matter Core Engine)             │  │
│  │  - Endpoint 1: Color Temperature Light (0x010C)              │  │
│  │  - Clusters: OnOff (0x0006), Level (0x0008), Color (0x0300)  │  │
│  │  - Non-Volatile Storage (NVS): custom_mac & Fabric State     │  │
│  └──────┬───────────────────────┬──────────────────────┬────────┘  │
│         │                       │                      │           │
│         ▼                       ▼                      ▼           │
│  ┌──────────────┐      ┌─────────────────┐    ┌─────────────────┐  │
│  │ Status Task  │      │   Button Task   │    │  LEDC Hardware  │  │
│  │   (GPIO 8)   │      │(GPIO 9 Debounce)│    │ (22kHz, 11-Bit) │  │
│  └──────┬───────┘      └────────▲────────┘    └───────┬───┬─────┘  │
└─────────┼───────────────────────┼─────────────────────┼───┼────────┘
          │                       │                     │   │
          │ GPIO 8 (Output)       │ GPIO 9 (Input)      │   │
          ▼                       │                     │   │
┌───────────────────┐    ┌────────┴────────┐            │   │
│    Status LED     │    │   Push Button   │            │   │
│  - Fast: Pairing  │    │   (BOOT / SW1)  │            │   │
│  - Off: Paired    │    │ - Short: Toggle │            │   │
│  - Pulse: Identify│    │ - 10s: Reset    │            │   │
└───────────────────┘    └─────────────────┘            │   │
                                                        │   │
                ┌───────────────────────────────────────┘   │
                │ GPIO 0 PWM (CW)           ┌───────────────┘
                │                           │ GPIO 1 PWM (WW)
                ▼                           ▼
  ┌───────────────────────────┐       ┌───────────────────────────┐
  │  N-MOSFET: Cool White CW  │       │  N-MOSFET: Warm White WW  │
  │  - Logic-Level (IRLZ44N)  │       │  - Logic-Level (IRLZ44N)  │
  │  - 100 Ω Gate / 10 kΩ PD  │       │  - 100 Ω Gate / 10 kΩ PD  │
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
| **Cool White (CW)** | `GPIO 0` | Active-High PWM | N-Channel MOSFET Gate | 22 kHz, 11-bit resolution |
| **Warm White (WW)** | `GPIO 1` | Active-High PWM | N-Channel MOSFET Gate | 22 kHz, 11-bit resolution |
| **Status LED** | `GPIO 8` | Active-Low Output | Current-limiting resistor (e.g. 1kΩ) | Fast blink = pairing mode; Off = paired |
| **Control Button** | `GPIO 9` | Active-Low Input | Internal pull-up to 3.3V | BOOT pin: Short press = toggle; 10s hold = reset |

### Recommended MOSFET Wiring
For driving 12V or 24V LED strips:
* **MOSFETs**: Logic-level N-Channel MOSFETs such as **IRLZ44N**, **AO3400A** (SMD), or **FQP30N06L**.
* **Gate Resistor**: $100\,\Omega$ series resistor between ESP32-C6 GPIO and MOSFET Gate.
* **Pull-Down Resistor**: $10\,\text{k}\Omega$ resistor from MOSFET Gate to GND (prevents LED flicker during bootloader stages).
* **Common Ground**: Ensure 12V/24V power supply GND is tied to ESP32-C6 GND.

---

## 2. Key Features

* **Matter Device Type**: `Color Temperature Light` (`0x010C`):
  * **OnOff Cluster**: Power control with non-volatile state restoration after power cycles.
  * **LevelControl Cluster**: Brightness control (1–254) using a smooth **$\gamma = 2.2$ gamma-corrected** logarithmic curve.
  * **ColorControl Cluster**: Tunable white from **2700K (370 Mireds)** to **6500K (153 Mireds)**.
* **Matter over Wi-Fi**: Directly connects to your standard 2.4 GHz Wi-Fi router. **No Thread Border Router is required.**
* **Custom Static MAC Address**:
  * Configurable via `idf.py menuconfig` (`CONFIG_CUSTOM_MAC_ADDRESS`).
  * Runtime override via NVS storage (namespace `custom_cfg`, key `custom_mac`).
  * Applied via `esp_base_mac_addr_set()` before Wi-Fi / BLE stacks initialize.
* **Dual OTA Partitions**: Standard Matter Over-The-Air Requestor cluster enabled for secure wireless firmware updates.

---

## 3. Migration from HomeKit Accessory Architect (HAA)

This project translates the legacy HAA JSON configuration into official Matter standard:

| HAA JSON Parameter | HAA Meaning | ESP32-C6 Matter Equivalent |
| :--- | :--- | :--- |
| `"b": [ [9, 5] ]` | Button on GPIO 9 | Handled by `app_driver.c` (Short-press toggle, 10s factory reset) |
| `"io": [ [[0, 1], 9] ]` | GPIO 0, 1 as PWM outputs | LEDC Channel 0 (GPIO 0: CW) & Channel 1 (GPIO 1: WW) |
| `"io": [ [[8], 2] ]` | GPIO 8 as output | Status LED driver (Active-Low) |
| `"l": 8` | Status LED on GPIO 8 | BLE Commissioning indication & identifying blink |
| `"y": [22000]` | 22 kHz PWM Frequency | Configured in LEDC Timer 0 (`CONFIG_PWM_FREQUENCY_HZ = 22000`) |
| `"t": 30, "ty": 1` | CCT Lightbulb Accessory | Matter `ESP_MATTER_DEVICE_COLOR_TEMPERATURE_LIGHT` (`0x010C`) |
| `"cm": [3, 4]` | Color Temp & Brightness | Matter `LevelControl` & `ColorControl` clusters |

---

## 4. How to Build & Flash

### Prerequisites
* ESP-IDF **v5.1.x** or **v5.2.x** installed.
* Espressif Matter Framework (`esp-matter`).

### 1. Set Target
```bash
idf.py set-target esp32c6
```

### 2. Configure (Optional)
To adjust the custom MAC address, GPIOs, or PWM frequency:
```bash
idf.py menuconfig
```
Navigate to:
> `CW-WW LED Controller Configuration` $\rightarrow$ `Custom MAC Address Configuration`

### 3. Build & Flash
```bash
idf.py build
idf.py -p COMx flash monitor
```
*(Replace `COMx` with your actual serial port, e.g., `COM3` on Windows or `/dev/ttyUSB0` on Linux/macOS).*

---

## 5. Pairing with Smart Home Ecosystems

1. Power on the ESP32-C6. The status LED (`GPIO 8`) will blink quickly (pairing mode).
2. The serial console will print the Matter QR code URL and the **11-digit Manual Pairing Code** (e.g., `3497-0112-332`).
3. Open your smart home app:
   * **Apple Home**: Tap `+` $\rightarrow$ **Add Accessory** $\rightarrow$ Scan QR or tap **More options...** and select the controller.
   * **Google Home**: Tap `+` $\rightarrow$ **Set up device** $\rightarrow$ **Matter-enabled device**.
   * **Home Assistant**: Go to **Settings** $\rightarrow$ **Devices & Services** $\rightarrow$ **Add Integration** $\rightarrow$ **Matter**.
4. Once paired, the status LED turns off, and the controller is ready for use!

---

## 6. Factory Reset

To unpair the device and clear all stored Wi-Fi credentials and Matter fabric certificates:
1. Press and hold the push button (`GPIO 9`) for **10 seconds**.
2. The controller will log the reset command, erase its Matter credentials, and reboot into BLE pairing mode (`GPIO 8` fast blink).

---

## License

This project is licensed under the [MIT License](LICENSE).

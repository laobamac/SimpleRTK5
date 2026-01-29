<div align="center">

# SimpleRTK5
### Realtek RTL8126 5GbE Driver for macOS

[![Platform](https://img.shields.io/badge/Platform-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)](https://www.apple.com/macos)
[![Chipset](https://img.shields.io/badge/Chipset-RTL8126-005696?style=for-the-badge&logo=realtek&logoColor=white)](https://www.realtek.com)
[![Speed](https://img.shields.io/badge/Speed-5GbE-76B900?style=for-the-badge&logo=speedtest&logoColor=white)]()
[![Language](https://img.shields.io/badge/Language-C++%20%7C%20Objective--C-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)]()
[![License](https://img.shields.io/badge/License-GPL_v3-red?style=for-the-badge)](LICENSE)
[![Build Status](https://img.shields.io/badge/Build-Passing-success?style=for-the-badge)]()

<p align="center">
  <b>SimpleRTK5</b> is a high-performance open-source kernel extension (kext) enabling support for the <b>Realtek RTL8126 5GbE</b> Ethernet controller on macOS.
  <br />
  Designed for Hackintosh builds and real Macs using PCIe adapters.
</p>

[中文 (Chinese)](README-CN.md)

</div>

---

<div id="english"></div>

##  English

### ✨ Features

* **Full 5GbE Support:** Native support for 5000Mbps link speeds, with auto-negotiation down to 2.5Gbps, 1Gbps, 100Mbps, and 10Mbps.
* **Hardware Offloading:** Supports TCP Segmentation Offload (TSO4/TSO6) and Checksum Offload (CSO6) for reduced CPU usage.
* **Energy Efficient:** Implements Energy Efficient Ethernet (EEE) support (configurable).
* **Native Integration:** Fully integrates with macOS `IONetworkingFamily`, supporting Network Preference Pane, jumbo frames, and multicast.
* **System Stability:** Optimized interrupt handling and memory management to prevent kernel panics.

### 🚀 Installation

#### For OpenCore Users (Recommended)

1.  Download the latest release of `SimpleRTK5.kext`.
2.  Copy the kext to your EFI partition: `EFI/OC/Kexts/`.
3.  Add the kext to your `config.plist` (Snapshot your folder using ProperTree or add manually).
4.  **Requirements:**
    * macOS Catalina (10.15) or newer (Tested on Sequoia).
    * `IOPCIFamily` and `IONetworkingFamily` (Standard in macOS).

#### Boot Arguments & Properties

You can customize the driver behavior by adding entries to `DeviceProperties` in your `config.plist` for the PCI path of your Ethernet card.

| Key | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `disableASPM` | Boolean | `true` | Disables Active State Power Management. Keep `true` for stability. |
| `enableEEE` | Boolean | `true` | Enables Energy Efficient Ethernet. Set `false` if you experience lag. |
| `enableTSO4` | Boolean | `true` | IPv4 TCP Segmentation Offload. |
| `enableTSO6` | Boolean | `true` | IPv6 TCP Segmentation Offload. |
| `enableCSO6` | Boolean | `true` | IPv6 Checksum Offload. |
| `µsPollInt2500`| Number | `110` | Polling interval tuning for high load. |

### 🛠 Building from Source

```bash
# Clone the repository
git clone https://github.com/laobamac/SimpleRTK5.git
cd SimpleRTK5

# Build using Xcode
xcodebuild -project SimpleRTK5.xcodeproj -configuration Release

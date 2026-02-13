<div align="center">

# SimpleRTK5
### Realtek RTL8125/8126 2.5/5GbE Driver for macOS

[![Platform](https://img.shields.io/badge/Platform-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)](https://www.apple.com/macos)
[![Chipset](https://img.shields.io/badge/Chipset-RTL8125/8126-005696?style=for-the-badge&logo=realtek&logoColor=white)](https://www.realtek.com)
[![Speed](https://img.shields.io/badge/Speed-2.5/5GbE-76B900?style=for-the-badge&logo=speedtest&logoColor=white)]()
[![Language](https://img.shields.io/badge/Language-C++%20%7C%20Objective--C-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)]()
[![License](https://img.shields.io/badge/License-GPL_v2-red?style=for-the-badge)](LICENSE)
[![Build Status](https://img.shields.io/badge/Build-Passing-success?style=for-the-badge)]()

<p align="center">
  <b>SimpleRTK5</b> is a high-performance open-source kernel extension (kext) enabling support for the <b>Realtek RTL8125/8126 2.5/5GbE</b> Ethernet controller on macOS.
  <br />
  Designed for Hackintosh builds and real Macs using PCIe adapters.
</p>

[中文 (Chinese)](README-CN.md)

</div>

---

## ✨ Features

* 🚀 **Native Support**: Fully compatible with macOS network stack.(Support AppleVTD)
* ⚡️ **High Speed**: Supports **2.5Gbps** (RTL8125 series) and **5Gbps** (RTL8126 series) link speeds.
* 🛠 **Advanced Config**: Supports ASPM (Active State Power Management) and TSO (TCP Segmentation Offload).
* 🔧 **Customizable**: Adjustable polling times for different link speeds via boot arguments or device properties.

## 🖥 Supported Hardware

This driver supports the following Realtek PCIe Ethernet Controllers:

| Chipset Series | Speed | PCI ID (Vendor:Device) |
| :--- | :--- | :--- |
| **RTL8125** | 2.5 Gbit/s | `0x10EC:0x8125`, `0x10EC:0x3000` |
| **RTL8126** | 5 Gbit/s | `0x10EC:0x8126`, `0x10EC:0x5000` |
| **RTL8125 (Killer)** | 2.5 Gbit/s | `0x1186:0x8125` |

## 📥 Installation

### OpenCore (Recommended)

1.  Download the latest release from the [Releases](https://github.com/laobamac/SimpleRTK5/releases) page.
2.  Copy `SimpleRTK5.kext` to your `EFI/OC/Kexts` folder.
3.  Add the kext entry to your `config.plist` (Kernel -> Add).
4.  **Optional**: Configure boot arguments if needed (see below).
5.  Reboot.

### Clover

1.  Download the latest release.
2.  Copy `SimpleRTK5.kext` to `EFI/CLOVER/kexts/Other`.
3.  Reboot.

## ⚙️ Configuration & Boot Arguments

You can customize the driver behavior using boot arguments or `DeviceProperties` in your bootloader config.

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `enableASPM` | Boolean | `True` | Enables Active State Power Management. Set to `False` if you experience instability. |
| `enableTSO4` | Boolean | `False` | Enables TCP Segmentation Offload for IPv4. |
| `enableTSO6` | Boolean | `False` | Enables TCP Segmentation Offload for IPv6. |
| `µsPollTime2G` | Integer | `160` | Polling interval (microseconds) for 2.5G connection. |
| `µsPollTime5G` | Integer | `120` | Polling interval (microseconds) for 5G connection. |

**Example Boot Argument:**
```bash
-srtk5noaspm   # (Hypothetical example if bool args are implemented as flags, otherwise use DeviceProperties)

```

*Note: It is recommended to set these values via `DeviceProperties` in OpenCore `config.plist` under the PCI path of your ethernet card.*

## 👏 Credits

* **Realtek** for the original Linux driver source code.
* **Laura Müller** for the initial porting work.

---

<p align="center">Made with ❤️ for the Hackintosh Community</p>
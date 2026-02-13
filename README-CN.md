<div align="center">

# SimpleRTK5
### 适用于macOS的Realtek RTL8125/8126 2.5/5GbE网卡驱动

[![平台](https://img.shields.io/badge/平台-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)](https://www.apple.com/macos)
[![芯片](https://img.shields.io/badge/芯片-RTL8125/8126-005696?style=for-the-badge&logo=realtek&logoColor=white)](https://www.realtek.com)
[![速率](https://img.shields.io/badge/速率-2.5/5GbE-76B900?style=for-the-badge&logo=speedtest&logoColor=white)]()
[![语言](https://img.shields.io/badge/语言-C++%20%7C%20Objective--C-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)]()
[![许可证](https://img.shields.io/badge/许可证-GPL_v2-red?style=for-the-badge)](LICENSE)
[![构建状态](https://img.shields.io/badge/构建-成功-success?style=for-the-badge)]()

<p align="center">
  <b>SimpleRTK5</b> 是一个高性能的开源内核扩展(kext)，为macOS系统提供对<b>Realtek RTL8125/8126 2.5/5GbE</b>以太网控制器的原生支持。
  <br />
  专为黑苹果(Hackintosh)系统和使用PCIe适配器的苹果真机设计。
</p>

[English](README.md)

</div>

---

## ✨ 功能特点

* 🚀 **原生支持**：完美兼容macOS网络协议栈（支持AppleVTD）
* ⚡️ **高速传输**：支持**2.5Gbps**（RTL8125系列）和**5Gbps**（RTL8126系列）连接速率
* 🛠 **高级配置**：支持ASPM（主动电源状态管理）和TSO（TCP分段卸载）
* 🔧 **灵活定制**：可通过引导参数或设备属性为不同连接速率调整轮询时间

## 🖥 支持的硬件

本驱动支持以下Realtek PCIe以太网控制器：

| 芯片系列 | 速率 | PCI ID（厂商:设备） |
| :--- | :--- | :--- |
| **RTL8125** | 2.5 Gbit/s | `0x10EC:0x8125`, `0x10EC:0x3000` |
| **RTL8126** | 5 Gbit/s | `0x10EC:0x8126`, `0x10EC:0x5000` |
| **RTL8125（Killer版）** | 2.5 Gbit/s | `0x1186:0x8125` |

## 📥 安装方法

### OpenCore（推荐）

1.  从[发布页面](https://github.com/laobamac/SimpleRTK5/releases)下载最新版本
2.  将 `SimpleRTK5.kext` 复制到 `EFI/OC/Kexts` 文件夹
3.  在 `config.plist` 中添加内核扩展条目（Kernel -> Add）
4.  **可选**：根据需要配置引导参数（见下文）
5.  重启系统

### Clover

1.  下载最新版本
2.  将 `SimpleRTK5.kext` 复制到 `EFI/CLOVER/kexts/Other`
3.  重启系统

## ⚙️ 配置与引导参数

您可以通过引导参数或引导配置文件中的 `DeviceProperties` 来自定义驱动程序行为。

| 参数 | 类型 | 默认值 | 描述 |
| :--- | :--- | :--- | :--- |
| `enableASPM` | 布尔值 | `True` | 启用主动电源状态管理。如遇不稳定情况可设为 `False` |
| `enableTSO4` | 布尔值 | `False` | 启用IPv4 TCP分段卸载 |
| `enableTSO6` | 布尔值 | `False` | 启用IPv6 TCP分段卸载 |
| `µsPollTime2G` | 整数 | `160` | 2.5G连接时的轮询间隔（微秒） |
| `µsPollTime5G` | 整数 | `120` | 5G连接时的轮询间隔（微秒） |

**引导参数示例：**
```bash
-srtk5noaspm   # （示例，如布尔参数通过标志实现；否则请使用设备属性设置）
```

*注意：建议在OpenCore的`config.plist`中通过`DeviceProperties`，在网卡对应的PCI路径下设置这些值。*

## 👏 致谢

* **Realtek** 提供原始的Linux驱动源代码
* **Laura Müller** 完成的初始移植工作
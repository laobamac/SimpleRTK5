# SimpleRTK5
macOS's RTL8126 5G Ethernet card driver

### ✨ 主要功能

* **原生 5GbE 支持：** 支持 5000Mbps 连接速率，并向下兼容 2.5Gbps、1Gbps、100Mbps 和 10Mbps。
* **硬件卸载 (Offloading)：** 支持 TCP 分段卸载 (TSO4/TSO6) 和校验和卸载 (CSO6)，显著降低 CPU 占用率。
* **节能以太网：** 支持 EEE (Energy Efficient Ethernet) 节能技术。
* **原生系统集成：** 基于 `IONetworkingFamily` 开发，支持系统“网络”偏好设置、巨型帧 (Jumbo Frames) 和多播。
* **高稳定性：** 优化的中断处理和内存管理，防止在高负载下系统崩溃。

### 🚀 安装指南

#### OpenCore 用户 (推荐)

1. 下载最新发布的 `SimpleRTK5.kext`。
2. 将驱动文件复制到 EFI 分区的 `EFI/OC/Kexts/` 目录下。
3. 在 `config.plist` 中启用该驱动（建议使用 ProperTree 进行 OC Snapshot）。
4. **系统要求：**
* macOS Catalina (10.15) 或更高版本（已在 macOS Sequoia 上测试）。



#### 驱动配置参数

您可以通过 OpenCore 的 `config.plist` 中的 `DeviceProperties` 为网卡路径添加以下参数来调整驱动行为：

| 键名 (Key) | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `disableASPM` | Boolean | `true` | 禁用活动状态电源管理。建议保持 `true` 以确保稳定性。 |
| `enableEEE` | Boolean | `true` | 启用节能以太网。如果遇到网络卡顿或断流，可尝试设为 `false`。 |
| `enableTSO4` | Boolean | `true` | 启用 IPv4 TCP 分段卸载。 |
| `enableTSO6` | Boolean | `true` | 启用 IPv6 TCP 分段卸载。 |
| `enableCSO6` | Boolean | `true` | 启用 IPv6 校验和卸载。 |
| `µsPollInt2500` | Number | `110` | 轮询间隔微秒数，用于优化高吞吐量下的性能。 |

### 🛠 源码编译

```bash
# 克隆项目仓库
git clone https://github.com/laobamac/SimpleRTK5.git
cd SimpleRTK5

# 使用 Xcode 编译
xcodebuild -project SimpleRTK5.xcodeproj -configuration Release

```

### 📝 致谢与版权

* **作者：** 王孝慈 (laobamac)
* **版权所有：** Copyright © 2025 王孝慈. All rights reserved.
* **核心参考：** 基于 Linux r8126 驱动源码移植与改进。

---

<div align="center">
<p>Made with ❤️ for the Hackintosh Community</p>
</div>

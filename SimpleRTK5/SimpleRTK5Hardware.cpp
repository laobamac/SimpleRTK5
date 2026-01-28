//
//  SimpleRTK5Hardware.cpp
//  SimpleRTK5
//
//  Created by laobamac on 2025/10/6.
//

#include "SimpleRTK5Ethernet.hpp"

#pragma mark--- 硬件初始化方法 ---

bool SimpleRTK5::initPCIConfigSpace(IOPCIDevice *provider)
{
    IOByteCount pmCapOffset;
    UInt32 pcieLinkCap;
    UInt16 pcieLinkCtl;
    UInt16 cmdReg;
    UInt16 pmCap;
    bool result = false;

    // 确保 provider 存在
    if (!provider) {
        return false;
    }

    // 读取 PCI 供应商和设备 ID
    pciDeviceData.vendor = provider->configRead16(kIOPCIConfigVendorID);
    pciDeviceData.device = provider->configRead16(kIOPCIConfigDeviceID);
    pciDeviceData.subsystem_vendor = provider->configRead16(kIOPCIConfigSubSystemVendorID);
    pciDeviceData.subsystem_device = provider->configRead16(kIOPCIConfigSubSystemID);

    // 配置电源管理功能
    if (provider->extendedFindPCICapability(kIOPCIPowerManagementCapability, &pmCapOffset))
    {
        pmCap = provider->extendedConfigRead16(pmCapOffset + kIOPCIPMCapability);
        DebugLog("SimpleRTK5: PCI 电源管理能力: 0x%x.\n", pmCap);

        // 检查是否支持从 D3Cold 状态唤醒
        if (pmCap & kPCIPMCPMESupportFromD3Cold)
        {
            wolCapable = true;
            DebugLog("SimpleRTK5: 支持从 D3 (cold) 唤醒 (PME#).\n");
        }
        pciPMCtrlOffset = pmCapOffset + kIOPCIPMControl;
    }
    else
    {
        IOLog("SimpleRTK5: 不支持 PCI 电源管理.\n");
    }
    
    // 启用 PCI 电源管理并设置为 D0 状态（全速运行）
    provider->enablePCIPowerManagement(kPCIPMCSPowerStateD0);

    // 获取并配置 PCIe 链路信息
    if (provider->extendedFindPCICapability(kIOPCIPCIExpressCapability, &pcieCapOffset))
    {
        pcieLinkCap = provider->configRead32(pcieCapOffset + kIOPCIELinkCapability);
        pcieLinkCtl = provider->configRead16(pcieCapOffset + kIOPCIELinkControl);
        DebugLog("SimpleRTK5: PCIe 链路能力: 0x%08x, 控制: 0x%04x.\n", pcieLinkCap, pcieLinkCtl);

        // 根据配置处理 ASPM (主动状态电源管理)
        if (linuxData.configASPM == 0)
        {
            IOLog("SimpleRTK5: 禁用 PCIe ASPM.\n");
            provider->setASPMState(this, 0);
        }
        else
        {
            IOLog("SimpleRTK5: 警告: 启用 PCIe ASPM.\n");
            provider->setASPMState(this, kIOPCIELinkCtlASPM | kIOPCIELinkCtlClkPM);
            linuxData.configASPM = 1;
        }
    }

    // 启用 PCI 设备总线主控和内存空间
    cmdReg = provider->configRead16(kIOPCIConfigCommand);
    cmdReg &= ~kIOPCICommandIOSpace;
    cmdReg |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace | kIOPCICommandMemWrInvalidate);
    provider->configWrite16(kIOPCIConfigCommand, cmdReg);

    // 映射内存映射 I/O (MMIO) 资源
    baseMap = provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapInhibitCache);

    if (!baseMap)
    {
        IOLog("SimpleRTK5: 区域 #2 不是 MMIO 资源，初始化中止.\n");
        return false;
    }

    // 获取虚拟地址映射
    baseAddr = reinterpret_cast<volatile void *>(baseMap->getVirtualAddress());
    linuxData.mmio_addr = baseAddr;
    result = true;

    return result;
}

IOReturn SimpleRTK5::setPowerStateWakeAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    SimpleRTK5 *ethCtlr = OSDynamicCast(SimpleRTK5, owner);
    
    // 安全检查：确保转换成功且偏移量有效
    if (!ethCtlr || !ethCtlr->pciPMCtrlOffset) {
        return kIOReturnSuccess;
    }

    IOPCIDevice *dev = ethCtlr->pciDevice;
    UInt8 offset = ethCtlr->pciPMCtrlOffset;
    UInt16 val16 = dev->extendedConfigRead16(offset);

    // 清除 PME 状态并设置电源状态为 D0 (唤醒)
    val16 &= ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);
    val16 |= kPCIPMCSPowerStateD0;

    dev->extendedConfigWrite16(offset, val16);
    
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setPowerStateSleepAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    SimpleRTK5 *ethCtlr = OSDynamicCast(SimpleRTK5, owner);
    
    if (!ethCtlr || !ethCtlr->pciPMCtrlOffset) {
        return kIOReturnSuccess;
    }

    IOPCIDevice *dev = ethCtlr->pciDevice;
    UInt8 offset = ethCtlr->pciPMCtrlOffset;
    UInt16 val16 = dev->extendedConfigRead16(offset);

    // 准备进入睡眠状态
    val16 &= ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);

    // 如果启用了网络唤醒 (WoL)，启用 PME
    if (ethCtlr->wolActive)
        val16 |= (kPCIPMCSPMEStatus | kPCIPMCSPMEEnable | kPCIPMCSPowerStateD3);
    else
        val16 |= kPCIPMCSPowerStateD3;

    dev->extendedConfigWrite16(offset, val16);
    
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::identifyChip()
{
    struct rtl8126_private *tp = &linuxData;
    IOReturn result = kIOReturnSuccess;
    u32 reg, val32;
    u32 ICVerID;

    // 读取传输配置寄存器以识别芯片版本
    val32 = ReadReg32(TxConfig);
    reg = val32 & 0x7c800000;
    ICVerID = val32 & 0x00700000;

    // 根据寄存器值判断硬件版本
    switch (reg)
    {
    case 0x64800000:
        if (ICVerID == 0x00000000)
        {
            tp->mcfg = CFG_METHOD_1;
        }
        else if (ICVerID == 0x100000)
        {
            tp->mcfg = CFG_METHOD_2;
        }
        else if (ICVerID == 0x200000)
        {
            tp->mcfg = CFG_METHOD_3;
        }
        else
        {
            tp->mcfg = CFG_METHOD_3;
            tp->HwIcVerUnknown = true;
        }
        tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;
        
    default:
        DebugLog("SimpleRTK5: 未知芯片版本 (%x)\n", reg);
        tp->mcfg = CFG_METHOD_DEFAULT;
        tp->HwIcVerUnknown = true;
        tp->efuse_ver = EFUSE_NOT_SUPPORT;
        result = kIOReturnError;
        break;
    }
    return result;
}

bool SimpleRTK5::initRTL8126()
{
    struct rtl8126_private *tp = &linuxData;
    UInt32 i;
    UInt8 macAddr[MAC_ADDR_LEN];
    bool result = false;

    // 步骤1：识别芯片
    if (identifyChip())
    {
        IOLog("SimpleRTK5: 发现不支持的芯片，终止初始化.\n");
        return false;
    }

    // 步骤2：初始化基础参数
    tp->eee_adv_t = eeeCap = (MDIO_EEE_100TX | MDIO_EEE_1000T);
    tp->phy_reset_enable = rtl8126_xmii_reset_enable;
    tp->phy_reset_pending = rtl8126_xmii_reset_pending;
    tp->max_jumbo_frame_size = rtl_chip_info[tp->chipset].jumbo_frame_sz;

    // 根据硬件版本配置特定参数
    switch (tp->mcfg)
    {
    case CFG_METHOD_1:
    case CFG_METHOD_2:
    case CFG_METHOD_3:
        tp->HwPkgDet = (rtl8126_mac_ocp_read(tp, 0xDC00) >> 3) & 0x07;
        tp->HwSuppNowIsOobVer = 1;
        tp->HwPcieSNOffset = 0x174;
        break;
    default:
        break;
    }

#ifdef ENABLE_REALWOW_SUPPORT
    rtl8126_get_realwow_hw_version(dev);
#endif

    // 处理 ASPM 相关偏移修正
    if (linuxData.configASPM && (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3))
    {
        tp->org_pci_offset_99 = csiFun0ReadByte(0x99) & ~(BIT_5 | BIT_6);
        tp->org_pci_offset_180 = csiFun0ReadByte(0x22c);
    }

    tp->org_pci_offset_80 = pciDevice->configRead8(0x80);
    tp->org_pci_offset_81 = pciDevice->configRead8(0x81);
    tp->use_timer_interrupt = true;

    // 设置最大链路速度
    tp->HwSuppMaxPhyLinkSpeed = 5000; // 5Gbps

    // 配置校验和与短包填充
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        tp->ShortPacketSwChecksum = TRUE;
        tp->UseSwPaddingShortPkt = TRUE;
    }

    // 配置魔术包版本
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        tp->HwSuppMagicPktVer = WAKEUP_MAGIC_PACKET_V3;
        tp->HwSuppLinkChgWakeUpVer = 3;
        tp->HwSuppD0SpeedUpVer = 1;
        tp->HwSuppCheckPhyDisableModeVer = 3;
    } else {
        tp->HwSuppMagicPktVer = WAKEUP_MAGIC_PACKET_NOT_SUPPORT;
    }

    // 配置 TX No Close 版本
    if (tp->mcfg == CFG_METHOD_1) {
        tp->HwSuppTxNoCloseVer = 4;
    } else if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3) {
        tp->HwSuppTxNoCloseVer = 5;
    }

    // 设置 TX 描述符掩码
    switch (tp->HwSuppTxNoCloseVer)
    {
    case 5:
    case 6:
        tp->MaxTxDescPtrMask = MAX_TX_NO_CLOSE_DESC_PTR_MASK_V4;
        break;
    case 4:
        tp->MaxTxDescPtrMask = MAX_TX_NO_CLOSE_DESC_PTR_MASK_V3;
        break;
    case 3:
        tp->MaxTxDescPtrMask = MAX_TX_NO_CLOSE_DESC_PTR_MASK_V2;
        break;
    default:
        tp->EnableTxNoClose = false;
        break;
    }

    if (tp->HwSuppTxNoCloseVer > 0)
        tp->EnableTxNoClose = true;

    // 设置 RAM 代码版本
    switch (tp->mcfg)
    {
    case CFG_METHOD_1: tp->sw_ram_code_ver = NIC_RAMCODE_VERSION_CFG_METHOD_1; break;
    case CFG_METHOD_2: tp->sw_ram_code_ver = NIC_RAMCODE_VERSION_CFG_METHOD_2; break;
    case CFG_METHOD_3: tp->sw_ram_code_ver = NIC_RAMCODE_VERSION_CFG_METHOD_3; break;
    }

    if (tp->HwIcVerUnknown)
    {
        tp->NotWrRamCodeToMicroP = true;
        tp->NotWrMcuPatchCode = true;
    }

    // 配置 MCU 和队列
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        tp->HwSuppMacMcuVer = 2;
        tp->MacMcuPageSize = RTL8126_MAC_MCU_PAGE_SIZE;
        tp->HwSuppNumTxQueues = 2;
        tp->HwSuppNumRxQueues = 4;
    } else {
        tp->HwSuppNumTxQueues = 1;
        tp->HwSuppNumRxQueues = 1;
    }

    // 初始化中断版本
    switch (tp->mcfg)
    {
    case CFG_METHOD_1: tp->HwSuppIsrVer = 2; break;
    case CFG_METHOD_2:
    case CFG_METHOD_3: tp->HwSuppIsrVer = 3; break;
    default: tp->HwSuppIsrVer = 1; break;
    }

    // 配置 PTP 和 RSS
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        tp->HwSuppPtpVer = 2;
        tp->HwSuppRssVer = 5;
        tp->HwSuppIndirTblEntries = 128;
    }

    // 配置中断缓解
    if (tp->mcfg == CFG_METHOD_1)
        tp->HwSuppIntMitiVer = 4;
    else if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3)
        tp->HwSuppIntMitiVer = 5;

    // 配置 TCAM
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        tp->HwSuppTcamVer = 2;
        tp->TcamNotValidReg = TCAM_NOTVALID_ADDR_V2;
        tp->TcamValidReg = TCAM_VALID_ADDR_V2;
        tp->TcamMaAddrcOffset = TCAM_MAC_ADDR_V2;
        tp->TcamVlanTagOffset = TCAM_VLAN_TAG_V2;
        tp->HwSuppExtendTallyCounterVer = 1;
    }

    // 读取自定义 LED 值和 WoL 选项
    tp->NicCustLedValue = ReadReg16(CustomLED);
    tp->wol_opts = rtl8126_get_hw_wol(tp);
    tp->wol_enabled = (tp->wol_opts) ? WOL_ENABLED : WOL_DISABLED;
    tp->max_jumbo_frame_size = rtl_chip_info[tp->chipset].jumbo_frame_sz;

    wolCapable = (tp->wol_enabled == WOL_ENABLED);
    tp->eee_adv_t = MDIO_EEE_1000T | MDIO_EEE_100TX | SUPPORTED_2500baseX_Full;

    // 步骤3：硬件复位和上电序列
    exitOOB();
    rtl8126_powerup_pll(tp);
    rtl8126_hw_init(tp);
    rtl8126_nic_reset(tp);

    // 获取 EEPROM 类型
    rtl8126_eeprom_type(tp);
    if (tp->eeprom_type == EEPROM_TYPE_93C46 || tp->eeprom_type == EEPROM_TYPE_93C56)
        rtl8126_set_eeprom_sel_low(tp);

    // 步骤4：读取 MAC 地址
    for (i = 0; i < MAC_ADDR_LEN; i++)
        macAddr[i] = ReadReg8(MAC0 + i);

    // 某些版本从备份寄存器读取 MAC
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        *(UInt32 *)&macAddr[0] = RTL_R32(tp, BACKUP_ADDR0_8125);
        *(UInt16 *)&macAddr[4] = RTL_R16(tp, BACKUP_ADDR1_8125);
    }

    // 验证并设置 MAC 地址
    if (is_valid_ether_addr((UInt8 *)macAddr))
    {
        IOLog("SimpleRTK5: 获取到 MAC 地址 %02x:%02x:%02x:%02x:%02x:%02x\n",
              macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
        rtl8126_rar_set(tp, macAddr);
    }
    else
    {
        IOLog("SimpleRTK5: 使用回退 MAC 地址.\n");
        rtl8126_rar_set(tp, fallBackMacAddr.bytes);
    }

    // 保存原始 MAC 地址
    for (i = 0; i < MAC_ADDR_LEN; i++)
    {
        currMacAddr.bytes[i] = ReadReg8(MAC0 + i);
        origMacAddr.bytes[i] = currMacAddr.bytes[i];
    }
    
    IOLog("SimpleRTK5: %s: (Chipset %d), MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
          rtl_chip_info[tp->chipset].name, tp->chipset,
          origMacAddr.bytes[0], origMacAddr.bytes[1],
          origMacAddr.bytes[2], origMacAddr.bytes[3],
          origMacAddr.bytes[4], origMacAddr.bytes[5]);

    // 步骤5：配置命令寄存器和中断掩码
    tp->cp_cmd = (ReadReg16(CPlusCmd) | RxChkSum);

    intrMaskRxTx = (SYSErr | LinkChg | RxDescUnavail | TxOK | RxOK);
    intrMaskTimer = (SYSErr | LinkChg | RxDescUnavail | PCSTimeout | RxOK);
    intrMask = intrMaskRxTx;

    // 获取 RxConfig 参数
    rxConfigReg = rtl_chip_info[tp->chipset].RCR_Cfg;
    rxConfigMask = rtl_chip_info[tp->chipset].RxConfigMask;

    // 重置统计计数器
    WriteReg32(CounterAddrHigh, (statPhyAddr >> 32));
    WriteReg32(CounterAddrLow, (statPhyAddr & 0x00000000ffffffff) | CounterReset);

    rtl8126_disable_rxdvgate(tp);
    IOLog("SimpleRTK5: 8126 初始化完成.\n");

#ifdef DEBUG
    if (wolCapable)
        IOLog("SimpleRTK5: 设备支持 WoL.\n");
#endif

    result = true;
    return result;
}

void SimpleRTK5::enableRTL8126()
{
    struct rtl8126_private *tp = &linuxData;

    setLinkStatus(kIONetworkLinkValid);

    intrMask = intrMaskRxTx;
    clear_bit(__POLL_MODE, &stateFlags);

    // 退出 OOB，初始化硬件，复位 NIC，上电 PLL
    exitOOB();
    rtl8126_hw_init(tp);
    rtl8126_nic_reset(tp);
    rtl8126_powerup_pll(tp);
    rtl8126_hw_ephy_config(tp);
    configPhyHardware();
    setupRTL8126();

    setPhyMedium();
}

void SimpleRTK5::disableRTL8126()
{
    struct rtl8126_private *tp = &linuxData;

    // 清除中断掩码以禁用所有中断
    WriteReg32(IMR0_8125, 0);

    // 复位并进入省电模式
    rtl8126_nic_reset(tp);
    hardwareD3Para();
    powerDownPLL();

    if (test_and_clear_bit(__LINK_UP, &stateFlags))
    {
        setLinkStatus(kIONetworkLinkValid);
        IOLog("SimpleRTK5: en%u 链路断开\n", netif->getUnitNumber());
    }
}

void SimpleRTK5::restartRTL8126()
{
    DebugLog("SimpleRTK5: 执行 restartRTL8126 ===>\n");

    // 停止输出线程并刷新队列
    if (netif) {
        netif->stopOutputThread();
        netif->flushOutputQueue();
    }

    clear_bit(__LINK_UP, &stateFlags);
    setLinkStatus(kIONetworkLinkValid);

    // 复位 NIC 并清理描述符环
    rtl8126_nic_reset(&linuxData);
    clearRxTxRings();

    // 重新初始化
    enableRTL8126();
}

void SimpleRTK5::setupRTL8126()
{
    struct rtl8126_private *tp = &linuxData;
    UInt16 mac_ocp_data;

    // 配置接收过滤器：拒绝错误、过短包，禁用多播等，准备复位
    WriteReg32(RxConfig, ReadReg32(RxConfig) & ~(AcceptErr | AcceptRunt | AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys));

    rtl8126_nic_reset(tp);

    // 解锁配置寄存器
    WriteReg8(Cfg9346, ReadReg8(Cfg9346) | Cfg9346_Unlock);

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        WriteReg8(0xF1, ReadReg8(0xF1) & ~BIT_7);
        WriteReg8(Config2, ReadReg8(Config2) & ~BIT_7);
        WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
    }

    // 仅保留魔术包配置
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xC0B6);
        mac_ocp_data &= BIT_0;
        rtl8126_mac_ocp_write(tp, 0xC0B6, mac_ocp_data);
    }

    if (tp->HwSuppExtendTallyCounterVer == 1) {
        rtl8126_set_mac_ocp_bit(tp, 0xEA84, (BIT_1 | BIT_0));
    }

    // 填充统计计数器物理地址
    WriteReg32(CounterAddrHigh, (statPhyAddr >> 32));
    WriteReg32(CounterAddrLow, (statPhyAddr & 0x00000000ffffffff));

    // 设置描述符环指针
    txTailPtr0 = txClosePtr0 = 0;
    txNextDescIndex = txDirtyDescIndex = 0;
    txNumFreeDesc = kNumTxDesc;
    rxNextDescIndex = 0;

    WriteReg32(TxDescStartAddrLow, (txPhyAddr & 0x00000000ffffffff));
    WriteReg32(TxDescStartAddrLow + 4, (txPhyAddr >> 32));
    WriteReg32(RxDescAddrLow, (rxPhyAddr & 0x00000000ffffffff));
    WriteReg32(RxDescAddrLow + 4, (rxPhyAddr >> 32));

    // 设置 DMA 突发大小和帧间隙
    WriteReg32(TxConfig, (TX_DMA_BURST_unlimited << TxDMAShift) | (InterFrameGap << TxInterFrameGapShift));

    if (tp->EnableTxNoClose)
        WriteReg32(TxConfig, (ReadReg32(TxConfig) | BIT_6));

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        WriteReg16(DOUBLE_VLAN_CONFIG, 0);
    }

    // 特定版本的大量寄存器初始化序列
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3)
    {
        set_offset70F(tp, 0x27);
        setOffset79(0x40);

        rtl8126_csi_write(tp, 0x890, rtl8126_csi_read(tp, 0x890) & ~BIT(0));

        // 禁用 RSS
        WriteReg32(RSS_CTRL_8125, 0x00);
        WriteReg16(Q_NUM_CTRL_8125, 0x0000);

        WriteReg8(Config1, ReadReg8(Config1) & ~0x10);

        rtl8126_mac_ocp_write(tp, 0xC140, 0xFFFF);
        rtl8126_mac_ocp_write(tp, 0xC142, 0xFFFF);

        // 处理 TX 描述符格式
        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xEB58);
        if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3)
            mac_ocp_data &= ~(BIT_0 | BIT_1);
        mac_ocp_data &= ~(BIT_0);
        rtl8126_mac_ocp_write(tp, 0xEB58, mac_ocp_data);

        WriteReg8(0xd8, ReadReg8(0xd8) & ~EnableRxDescV4_0);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xE614);
        mac_ocp_data &= ~(BIT_10 | BIT_9 | BIT_8);
        if (tp->EnableTxNoClose)
            mac_ocp_data |= (4 << 8);
        else
            mac_ocp_data |= (3 << 8);

        rtl8126_mac_ocp_write(tp, 0xE614, mac_ocp_data);

        // 设置 TX 队列数为 1
        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xE63E);
        mac_ocp_data &= ~(BIT_11 | BIT_10);
        mac_ocp_data |= ((0 & 0x03) << 10);
        rtl8126_mac_ocp_write(tp, 0xE63E, mac_ocp_data);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xE63E);
        mac_ocp_data &= ~(BIT_5 | BIT_4);
        mac_ocp_data |= ((0x02 & 0x03) << 4);
        rtl8126_mac_ocp_write(tp, 0xE63E, mac_ocp_data);

        rtl8126_clear_mac_ocp_bit(tp, 0xC0B4, BIT_0);
        rtl8126_set_mac_ocp_bit(tp, 0xC0B4, BIT_0);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xC0B4);
        mac_ocp_data |= (BIT_3 | BIT_2);
        rtl8126_mac_ocp_write(tp, 0xC0B4, mac_ocp_data);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xEB6A);
        mac_ocp_data &= ~(BIT_7 | BIT_6 | BIT_5 | BIT_4 | BIT_3 | BIT_2 | BIT_1 | BIT_0);
        mac_ocp_data |= (BIT_5 | BIT_4 | BIT_1 | BIT_0);
        rtl8126_mac_ocp_write(tp, 0xEB6A, mac_ocp_data);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xEB50);
        mac_ocp_data &= ~(BIT_9 | BIT_8 | BIT_7 | BIT_6 | BIT_5);
        mac_ocp_data |= (BIT_6);
        rtl8126_mac_ocp_write(tp, 0xEB50, mac_ocp_data);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xE056);
        mac_ocp_data &= ~(BIT_7 | BIT_6 | BIT_5 | BIT_4);
        rtl8126_mac_ocp_write(tp, 0xE056, mac_ocp_data);

        WriteReg8(TDFNR, 0x10);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xE040);
        mac_ocp_data &= ~(BIT_12);
        rtl8126_mac_ocp_write(tp, 0xE040, mac_ocp_data);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xEA1C);
        mac_ocp_data &= ~(BIT_1 | BIT_0);
        mac_ocp_data |= (BIT_0);
        rtl8126_mac_ocp_write(tp, 0xEA1C, mac_ocp_data);

        rtl8126_mac_ocp_write(tp, 0xE0C0, 0x4000);

        rtl8126_set_mac_ocp_bit(tp, 0xE052, (BIT_6 | BIT_5));
        rtl8126_clear_mac_ocp_bit(tp, 0xE052, BIT_3 | BIT_7);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xD430);
        mac_ocp_data &= ~(BIT_11 | BIT_10 | BIT_9 | BIT_8 | BIT_7 | BIT_6 | BIT_5 | BIT_4 | BIT_3 | BIT_2 | BIT_1 | BIT_0);
        mac_ocp_data |= 0x45F;
        rtl8126_mac_ocp_write(tp, 0xD430, mac_ocp_data);

        WriteReg8(0xD0, ReadReg8(0xD0) | BIT_6 | BIT_7);

        rtl8126_disable_eee_plus(tp);

        mac_ocp_data = rtl8126_mac_ocp_read(tp, 0xEA1C);
        mac_ocp_data &= ~(BIT_2);
        if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3)
            mac_ocp_data &= ~(BIT_9 | BIT_8);
        rtl8126_mac_ocp_write(tp, 0xEA1C, mac_ocp_data);

        rtl8126_clear_tcam_entries(tp);

        WriteReg16(0x1880, ReadReg16(0x1880) & ~(BIT_4 | BIT_5));
    }

    // 清理其他硬件参数
    rtl8126_hw_clear_timer_int(tp);
    rtl8126_hw_clear_int_miti(tp);
    rtl8126_enable_exit_l1_mask(tp);

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_mac_ocp_write(tp, 0xE098, 0xC302);
        
        if (linuxData.configASPM) {
            initPCIOffset99();
            rtl8126_init_pci_offset_180(tp);
        }
    }

    tp->cp_cmd &= ~(EnableBist | Macdbgo_oe | Force_halfdup |
                    Force_rxflow_en | Force_txflow_en | Cxpl_dbg_sel |
                    ASF | Macdbgo_sel);

    WriteReg16(CPlusCmd, tp->cp_cmd);

    // 设置最大 RX 缓冲区大小 (必须能容纳一个包)
    WriteReg16(RxMaxSize, rxBufferSize - 1);

    rtl8126_disable_rxdvgate(tp);

    // 设置接收模式 (多播/广播)
    setMulticastMode(test_bit(__M_CAST, &stateFlags));

    // 根据 ASPM 配置更新配置寄存器
    switch (tp->mcfg)
    {
    case CFG_METHOD_1:
        if (linuxData.configASPM)
        {
            RTL_W8(tp, Config2, RTL_R8(tp, Config2) | BIT_7);
            RTL_W8(tp, Config5, RTL_R8(tp, Config5) | BIT_0);
        }
        else
        {
            RTL_W8(tp, Config2, RTL_R8(tp, Config2) & ~BIT_7);
            RTL_W8(tp, Config5, RTL_R8(tp, Config5) & ~BIT_0);
        }
        break;
    case CFG_METHOD_2:
    case CFG_METHOD_3:
        if (linuxData.configASPM)
        {
            RTL_W8(tp, INT_CFG0_8125, RTL_R8(tp, INT_CFG0_8125) | BIT_3);
            RTL_W8(tp, Config5, RTL_R8(tp, Config5) | BIT_0);
        }
        else
        {
            RTL_W8(tp, INT_CFG0_8125, RTL_R8(tp, INT_CFG0_8125) & ~BIT_3);
            RTL_W8(tp, Config5, RTL_R8(tp, Config5) & ~BIT_0);
        }
        break;
    }

    // 锁定配置寄存器
    WriteReg8(Cfg9346, ReadReg8(Cfg9346) & ~Cfg9346_Unlock);

    // 启用所有已知中断
    WriteReg32(IMR0_8125, intrMask);

    DebugLog("SimpleRTK5: setup 完成 <===\n");
    udelay(10);
}

void SimpleRTK5::setPhyMedium()
{
    struct rtl8126_private *tp = netdev_priv(&linuxData);
    int auto_nego = 0;
    int giga_ctrl = 0;
    int ctrl_2500 = 0;

    // 如果不是固定速度，则启用自协商
    if (speed != SPEED_2500 && (speed != SPEED_1000) &&
        (speed != SPEED_100) && (speed != SPEED_10) && (speed != SPEED_5000))
    {
        duplex = DUPLEX_FULL;
        autoneg = AUTONEG_ENABLE;
    }

    // 处理节能以太网 (EEE)
    if ((linuxData.eee_adv_t != 0) && (autoneg == AUTONEG_ENABLE))
    {
        rtl8126_enable_eee(tp);
        DebugLog("SimpleRTK5: 启用 EEE 支持.\n");
    }
    else
    {
        rtl8126_disable_eee(tp);
        DebugLog("SimpleRTK5: 禁用 EEE 支持.\n");
    }

    // 禁用 Giga Lite
    rtl8126_clear_eth_phy_ocp_bit(tp, 0xA428, BIT_9);
    rtl8126_clear_eth_phy_ocp_bit(tp, 0xA5EA, BIT_0 | BIT_1 | BIT_2);

    // 读取当前控制寄存器
    giga_ctrl = rtl8126_mdio_read(tp, MII_CTRL1000);
    giga_ctrl &= ~(ADVERTISE_1000HALF | ADVERTISE_1000FULL);
    ctrl_2500 = rtl8126_mdio_direct_read_phy_ocp(tp, 0xA5D4);
    ctrl_2500 &= ~(RTK_ADVERTISE_2500FULL | RTK_ADVERTISE_5000FULL);

    auto_nego = rtl8126_mdio_read(tp, MII_ADVERTISE);
    auto_nego &= ~(ADVERTISE_10HALF | ADVERTISE_10FULL |
                   ADVERTISE_100HALF | ADVERTISE_100FULL |
                   ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);

    // 配置通告能力
    if (autoneg == AUTONEG_ENABLE)
    {
        // 默认全能通告
        auto_nego |= (ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL);
        giga_ctrl |= ADVERTISE_1000FULL;
        ctrl_2500 |= (RTK_ADVERTISE_2500FULL | RTK_ADVERTISE_5000FULL);
    }
    else if (speed == SPEED_5000)
    {
        ctrl_2500 |= RTK_ADVERTISE_5000FULL;
    }
    else if (speed == SPEED_2500)
    {
        ctrl_2500 |= RTK_ADVERTISE_2500FULL;
    }
    else if (speed == SPEED_1000)
    {
        if (duplex == DUPLEX_HALF)
            giga_ctrl |= ADVERTISE_1000HALF;
        else
            giga_ctrl |= ADVERTISE_1000FULL;
    }
    else if (speed == SPEED_100)
    {
        if (duplex == DUPLEX_HALF)
            auto_nego |= ADVERTISE_100HALF;
        else
            auto_nego |= ADVERTISE_100FULL;
    }
    else // SPEED_10
    {
        if (duplex == DUPLEX_HALF)
            auto_nego |= ADVERTISE_10HALF;
        else
            auto_nego |= ADVERTISE_10FULL;
    }

    // 设置流控制
    if (flowCtl == kFlowControlOn)
        auto_nego |= (ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);

    tp->phy_auto_nego_reg = auto_nego;
    tp->phy_1000_ctrl_reg = giga_ctrl;
    tp->phy_2500_ctrl_reg = ctrl_2500;

    // 写入 PHY 寄存器并重启 N-Way 自协商
    rtl8126_mdio_write(tp, 0x1f, 0x0000);
    rtl8126_mdio_write(tp, MII_ADVERTISE, auto_nego);
    rtl8126_mdio_write(tp, MII_CTRL1000, giga_ctrl);
    rtl8126_mdio_direct_write_phy_ocp(tp, 0xA5D4, ctrl_2500);
    rtl8126_phy_restart_nway(tp);
    mdelay(20);

    tp->autoneg = AUTONEG_ENABLE;
    tp->speed = speed;
    tp->duplex = duplex;
}

void SimpleRTK5::setOffset79(UInt8 setting)
{
    UInt8 deviceControl;
    DebugLog("SimpleRTK5: 设置 Offset79 ===>\n");

    if (!(linuxData.hwoptimize & HW_PATCH_SOC_LAN))
    {
        deviceControl = pciDevice->configRead8(0x79);
        deviceControl &= ~0x70;
        deviceControl |= setting;
        pciDevice->configWrite8(0x79, deviceControl);
    }
}

UInt8 SimpleRTK5::csiFun0ReadByte(UInt32 addr)
{
    struct rtl8126_private *tp = &linuxData;
    UInt8 retVal = 0;

    if (tp->mcfg == CFG_METHOD_DEFAULT)
    {
        retVal = pciDevice->configRead8(addr);
    }
    else
    {
        UInt32 tmpUlong;
        UInt8 shiftByte;

        shiftByte = addr & (0x3);
        tmpUlong = rtl8126_csi_other_fun_read(&linuxData, 0, addr);
        tmpUlong >>= (8 * shiftByte);
        retVal = (UInt8)tmpUlong;
    }
    udelay(20);

    return retVal;
}

void SimpleRTK5::csiFun0WriteByte(UInt32 addr, UInt8 value)
{
    struct rtl8126_private *tp = &linuxData;

    if (tp->mcfg == CFG_METHOD_DEFAULT)
    {
        pciDevice->configWrite8(addr, value);
    }
    else
    {
        UInt32 tmpUlong;
        UInt16 regAlignAddr;
        UInt8 shiftByte;

        regAlignAddr = addr & ~(0x3);
        shiftByte = addr & (0x3);
        tmpUlong = rtl8126_csi_other_fun_read(&linuxData, 0, regAlignAddr);
        tmpUlong &= ~(0xFF << (8 * shiftByte));
        tmpUlong |= (value << (8 * shiftByte));
        rtl8126_csi_other_fun_write(&linuxData, 0, regAlignAddr, tmpUlong);
    }
    udelay(20);
}

void SimpleRTK5::enablePCIOffset99()
{
    struct rtl8126_private *tp = &linuxData;
    u32 csi_tmp;

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        csiFun0WriteByte(0x99, linuxData.org_pci_offset_99);
        
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE032);
        csi_tmp &= ~(BIT_0 | BIT_1);
        if (tp->org_pci_offset_99 & (BIT_5 | BIT_6))
            csi_tmp |= BIT_1;
        if (tp->org_pci_offset_99 & BIT_2)
            csi_tmp |= BIT_0;
        rtl8126_mac_ocp_write(tp, 0xE032, csi_tmp);
    }
}

void SimpleRTK5::disablePCIOffset99()
{
    struct rtl8126_private *tp = &linuxData;

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_mac_ocp_write(tp, 0xE032, rtl8126_mac_ocp_read(tp, 0xE032) & ~(BIT_0 | BIT_1));
        csiFun0WriteByte(0x99, 0x00);
    }
}

void SimpleRTK5::initPCIOffset99()
{
    struct rtl8126_private *tp = &linuxData;
    u32 csi_tmp;

    switch (tp->mcfg)
    {
    case CFG_METHOD_1:
        rtl8126_mac_ocp_write(tp, 0xCDD0, 0x9003);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE034);
        csi_tmp |= (BIT_15 | BIT_14);
        rtl8126_mac_ocp_write(tp, 0xE034, csi_tmp);
        rtl8126_mac_ocp_write(tp, 0xCDD2, 0x889C);
        rtl8126_mac_ocp_write(tp, 0xCDD8, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDD4, 0x8C30);
        rtl8126_mac_ocp_write(tp, 0xCDDA, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDD6, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDDC, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDE8, 0x883E);
        rtl8126_mac_ocp_write(tp, 0xCDEA, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDEC, 0x889C);
        rtl8126_mac_ocp_write(tp, 0xCDEE, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDF0, 0x8C09);
        rtl8126_mac_ocp_write(tp, 0xCDF2, 0x9003);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE032);
        csi_tmp |= (BIT_14);
        rtl8126_mac_ocp_write(tp, 0xE032, csi_tmp);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE0A2);
        csi_tmp |= (BIT_0);
        rtl8126_mac_ocp_write(tp, 0xE0A2, csi_tmp);
        break;
        
    case CFG_METHOD_2:
    case CFG_METHOD_3:
        rtl8126_mac_ocp_write(tp, 0xCDD0, 0x9003);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE034);
        csi_tmp |= (BIT_15 | BIT_14);
        rtl8126_mac_ocp_write(tp, 0xE034, csi_tmp);
        rtl8126_mac_ocp_write(tp, 0xCDD2, 0x8C09);
        rtl8126_mac_ocp_write(tp, 0xCDD8, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDD4, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDDA, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDD6, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDDC, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDE8, 0x88FA);
        rtl8126_mac_ocp_write(tp, 0xCDEA, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDEC, 0x8C09);
        rtl8126_mac_ocp_write(tp, 0xCDEE, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDF0, 0x8A62);
        rtl8126_mac_ocp_write(tp, 0xCDF2, 0x9003);
        rtl8126_mac_ocp_write(tp, 0xCDF4, 0x883E);
        rtl8126_mac_ocp_write(tp, 0xCDF6, 0x9003);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE032);
        csi_tmp |= (BIT_14);
        rtl8126_mac_ocp_write(tp, 0xE032, csi_tmp);
        csi_tmp = rtl8126_mac_ocp_read(tp, 0xE0A2);
        csi_tmp |= (BIT_0);
        rtl8126_mac_ocp_write(tp, 0xE0A2, csi_tmp);
        break;
    }
    enablePCIOffset99();
}

void SimpleRTK5::setPCI99_180ExitDriverPara()
{
    struct rtl8126_private *tp = &linuxData;

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_issue_offset_99_event(tp);
        disablePCIOffset99();
        rtl8126_disable_pci_offset_180(tp);
    }
}

void SimpleRTK5::setPCI99_ExitDriverPara()
{
    struct rtl8126_private *tp = &linuxData;

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_issue_offset_99_event(tp);
        disablePCIOffset99();
    }
}

void SimpleRTK5::hardwareD3Para()
{
    struct rtl8126_private *tp = &linuxData;

    // 设置最大接收大小
    WriteReg16(RxMaxSize, RX_BUF_SIZE);
    WriteReg8(0xF1, ReadReg8(0xF1) & ~BIT_7);

    // 解锁并配置 Config 寄存器以准备睡眠
    WriteReg8(Cfg9346, ReadReg8(Cfg9346) | Cfg9346_Unlock);
    
    switch (tp->mcfg)
    {
    case CFG_METHOD_1:
        WriteReg8(Config2, ReadReg8(Config2) & ~BIT_7);
        WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
        break;
    case CFG_METHOD_2:
    case CFG_METHOD_3:
        WriteReg8(INT_CFG0_8125, ReadReg8(INT_CFG0_8125) & ~BIT_3);
        WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
        break;
    }
    WriteReg8(Cfg9346, ReadReg8(Cfg9346) & ~Cfg9346_Unlock);

    rtl8126_disable_exit_l1_mask(tp);
    setPCI99_ExitDriverPara();
    rtl8126_disable_rxdvgate(tp);

    if (tp->HwSuppExtendTallyCounterVer == 1) {
        rtl8126_clear_mac_ocp_bit(tp, 0xEA84, (BIT_1 | BIT_0));
    }
}

UInt16 SimpleRTK5::getEEEMode()
{
    struct rtl8126_private *tp = &linuxData;
    UInt16 eee = 0;
    UInt16 sup, adv, lpa, ena;

    if (eeeCap)
    {
        // 获取支持、通告、链路伙伴和启用的 EEE 状态
        sup = rtl8126_mdio_direct_read_phy_ocp(tp, 0xA5C4);
        DebugLog("SimpleRTK5: EEE 支持: %u\n", sup);

        adv = rtl8126_mdio_direct_read_phy_ocp(tp, 0xA5D0);
        DebugLog("SimpleRTK5: EEE 通告: %u\n", adv);

        lpa = rtl8126_mdio_direct_read_phy_ocp(tp, 0xA5D2);
        DebugLog("SimpleRTK5: EEE 链路伙伴: %u\n", lpa);

        ena = rtl8126_mac_ocp_read(tp, 0xE040);
        ena &= BIT_1 | BIT_0;
        DebugLog("SimpleRTK5: EEE 启用状态: %u\n", ena);

        eee = (sup & adv & lpa);
    }
    return eee;
}

void SimpleRTK5::exitOOB()
{
    struct rtl8126_private *tp = &linuxData;
    UInt16 data16;

    WriteReg32(RxConfig, ReadReg32(RxConfig) & ~(AcceptErr | AcceptRunt | AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys));

    // 禁用 RealWoL
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_mac_ocp_write(tp, 0xC0BC, 0x00FF);
    }

    rtl8126_nic_reset(tp);

    // 禁用 Now_is_oob 状态并配置 fifo
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        rtl8126_disable_now_is_oob(tp);

        data16 = rtl8126_mac_ocp_read(tp, 0xE8DE) & ~BIT_14;
        rtl8126_mac_ocp_write(tp, 0xE8DE, data16);
        rtl8126_wait_ll_share_fifo_ready(tp);

        rtl8126_mac_ocp_write(tp, 0xC0AA, 0x07D0);
        rtl8126_mac_ocp_write(tp, 0xC0A6, 0x01B5);
        rtl8126_mac_ocp_write(tp, 0xC01E, 0x5555);

        rtl8126_wait_ll_share_fifo_ready(tp);
    }

    // 等待 UPS 恢复
    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        if (rtl8126_is_ups_resume(tp))
        {
            rtl8126_wait_phy_ups_resume(tp, 2);
            rtl8126_clear_ups_resume_bit(tp);
            rtl8126_clear_phy_ups_reg(tp);
        }
    }
}

void SimpleRTK5::powerDownPLL()
{
    struct rtl8126_private *tp = &linuxData;

    // 如果启用了 WoL 或 KCP Offload，保持部分电路活动
    if (tp->wol_enabled == WOL_ENABLED || tp->EnableKCPOffload)
    {
        int auto_nego;
        int giga_ctrl;
        u16 anlpar;

        tp->check_keep_link_speed = 0;
        rtl8126_set_hw_wol(tp, tp->wol_opts);

        if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
            WriteReg8(Cfg9346, ReadReg8(Cfg9346) | Cfg9346_Unlock);
            WriteReg8(Config2, ReadReg8(Config2) | PMSTS_En);
            WriteReg8(Cfg9346, ReadReg8(Cfg9346) & ~Cfg9346_Unlock);
        }

        // 重置 MDIO 并配置通告能力以便在休眠期间保持连接
        rtl8126_mdio_write(tp, 0x1F, 0x0000);
        auto_nego = rtl8126_mdio_read(tp, MII_ADVERTISE);
        auto_nego &= ~(ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL);
        
        if (test_bit(__LINK_UP, &stateFlags))
            anlpar = tp->phy_reg_anlpar;
        else
            anlpar = rtl8126_mdio_read(tp, MII_LPA);

        if (anlpar & (LPA_10HALF | LPA_10FULL))
            auto_nego |= (ADVERTISE_10HALF | ADVERTISE_10FULL);
        else
            auto_nego |= (ADVERTISE_100FULL | ADVERTISE_100HALF | ADVERTISE_10HALF | ADVERTISE_10FULL);

        giga_ctrl = rtl8126_mdio_read(tp, MII_CTRL1000) & ~(ADVERTISE_1000HALF | ADVERTISE_1000FULL);
        rtl8126_mdio_write(tp, MII_ADVERTISE, auto_nego);
        rtl8126_mdio_write(tp, MII_CTRL1000, giga_ctrl);

        if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3)
        {
            int ctrl_2500;
            ctrl_2500 = rtl8126_mdio_direct_read_phy_ocp(tp, 0xA5D4);
            ctrl_2500 &= ~(RTK_ADVERTISE_2500FULL | RTK_ADVERTISE_5000FULL);
            rtl8126_mdio_direct_write_phy_ocp(tp, 0xA5D4, ctrl_2500);
        }
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xA428, BIT_9);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xA5EA, BIT_0 | BIT_1 | BIT_2);
        rtl8126_phy_restart_nway(tp);

        WriteReg32(RxConfig, ReadReg32(RxConfig) | AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        return;
    }

    // 彻底关闭 PHY 电源
    rtl8126_phy_power_down(tp);

    if (tp->mcfg >= CFG_METHOD_1 && tp->mcfg <= CFG_METHOD_3) {
        WriteReg8(PMCH, ReadReg8(PMCH) & ~BIT_7);
        WriteReg8(0xF2, ReadReg8(0xF2) & ~BIT_6);
    }
}

void SimpleRTK5::configPhyHardware()
{
    struct rtl8126_private *tp = &linuxData;

    if (tp->resume_not_chg_speed)
        return;

    // 复位并初始化 PHY MCU
    tp->phy_reset_enable(tp);
    rtl8126_init_hw_phy_mcu(tp);

    // 根据版本调用具体的 PHY 配置序列
    switch (tp->mcfg)
    {
    case CFG_METHOD_1:
        configPhyHardware8126a1();
        break;
    case CFG_METHOD_2:
        configPhyHardware8126a2();
        break;
    case CFG_METHOD_3:
        configPhyHardware8126a3();
        break;
    }

    // 清除传统强制模式位
    rtl8126_clear_eth_phy_ocp_bit(tp, 0xA5B4, BIT_15);

    rtl8126_mdio_write(tp, 0x1F, 0x0000);

    if (HW_HAS_WRITE_PHY_MCU_RAM_CODE(tp))
    {
        if (tp->eee_enabled == 1)
            rtl8126_enable_eee(tp);
        else
            rtl8126_disable_eee(tp);
    }
}

void SimpleRTK5::configPhyHardware8126a1()
{
        struct rtl8126_private *tp = &linuxData;

        rtl8126_set_eth_phy_ocp_bit(tp, 0xA442, BIT_11);
        // todo
        if (aspm)
        {
                if (HW_HAS_WRITE_PHY_MCU_RAM_CODE(tp))
                {
                        rtl8126_enable_phy_aldps(tp);
                }
        }
}

void SimpleRTK5::configPhyHardware8126a2()
{
        struct rtl8126_private *tp = &linuxData;

        rtl8126_set_eth_phy_ocp_bit(tp, 0xA442, BIT_11);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80BF);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0xED00);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80CD);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x1000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80D1);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0xC800);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80D4);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0xC800);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80E1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x10CC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x80E5);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x4F0C);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8387);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x4700);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA80C,
                                              BIT_7 | BIT_6,
                                              BIT_7);

        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAC90, BIT_4);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAD2C, BIT_15);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8321);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1100);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xACF8, (BIT_3 | BIT_2));
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8183);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x5900);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD94, BIT_5);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xA654, BIT_11);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xB648, BIT_14);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x839E);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x2F00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83F2);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0800);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xADA0, BIT_1);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x80F3);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x9900);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8126);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0xC100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x893A);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x8080);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8647);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0xE600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x862C);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1200);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x864A);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0xE600);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x80A0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0xBCBC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x805E);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0xBCBC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8056);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x3077);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8058);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x5A00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8098);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x3077);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x809A);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x5A00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8052);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x3733);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8094);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x3733);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x807F);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x7C75);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x803D);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x7C75);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8036);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8078);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8031);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3300);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8073);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3300);

        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xAE06,
                                              0xFC00,
                                              0x7C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x89D1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0004);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8FBD);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x0A00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8FBE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0D09);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x89CD);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0F0F);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x89CF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0F0F);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83A4);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83A6);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6601);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83C0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83C2);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6601);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8414);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8416);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6601);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83F8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x83FA);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x6601);

        rtl8126_set_phy_mcu_patch_request(tp);

        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xBD96,
                                              0x1F00,
                                              0x1000);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xBF1C,
                                              0x0007,
                                              0x0007);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xBFBE, BIT_15);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xBF40,
                                              0x0380,
                                              0x0280);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xBF90,
                                              BIT_7,
                                              (BIT_6 | BIT_5));
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xBF90,
                                              BIT_4,
                                              BIT_3 | BIT_2);

        rtl8126_clear_phy_mcu_patch_request(tp);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x843B);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x2000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x843D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x2000);

        rtl8126_clear_eth_phy_ocp_bit(tp, 0xB516, 0x7F);

        rtl8126_clear_eth_phy_ocp_bit(tp, 0xBF80, (BIT_5 | BIT_4));

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8188);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0044);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00A8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00D6);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00EC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00F6);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00FC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00BC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0058);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x002A);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8015);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0800);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFD);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFF);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x7F00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFB);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FE9);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0002);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FEF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x00A5);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FF1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0106);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FE1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0102);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FE3);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0400);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xA654, BIT_11);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0XA65A, (BIT_1 | BIT_0));

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xAC3A, 0x5851);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0XAC3C,
                                              BIT_15 | BIT_14 | BIT_12,
                                              BIT_13);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xAC42,
                                              BIT_9,
                                              BIT_8 | BIT_7 | BIT_6);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAC3E, BIT_15 | BIT_14 | BIT_13);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAC42, BIT_5 | BIT_4 | BIT_3);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xAC42,
                                              BIT_1,
                                              BIT_2 | BIT_0);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xAC1A, 0x00DB);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xADE4, 0x01B5);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAD9C, BIT_11 | BIT_10);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814B);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814F);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0B00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8142);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8144);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8150);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8118);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0700);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811A);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0700);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811C);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0500);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x810F);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8111);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xAC36, BIT_12);
        rtl8126_clear_eth_phy_ocp_bit(tp, 0xAD1C, BIT_8);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xADE8,
                                              0xFFC0,
                                              0x1400);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x864B);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x9D00);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8F97);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x003F);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x3F02);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x023C);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x3B0A);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x1C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD9C, BIT_5);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8122);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0C00);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x82C8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0009);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000B);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0021);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F7);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03B8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0049);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0049);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03B8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F7);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0021);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000B);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0009);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x80EF);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x82A0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000E);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0006);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x001A);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03D8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0023);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0054);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0322);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x00DD);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03AB);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03DC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0027);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000E);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E5);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F9);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0012);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0001);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F1);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8018);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xA438, BIT_13);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FE4);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0000);

        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB54C,
                                              0xFFC0,
                                              0x3700);

        if (aspm)
        {
                if (HW_HAS_WRITE_PHY_MCU_RAM_CODE(tp))
                {
                        rtl8126_enable_phy_aldps(tp);
                }
        }
}

void SimpleRTK5::configPhyHardware8126a3()
{
        struct rtl8126_private *tp = &linuxData;

        rtl8126_set_eth_phy_ocp_bit(tp, 0xA442, BIT_11);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8183);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x5900);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xA654, BIT_11);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xB648, BIT_14);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD2C, BIT_15);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD94, BIT_5);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xADA0, BIT_1);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xAE06,
                                              BIT_15 | BIT_14 | BIT_13 | BIT_12 | BIT_11 | BIT_10,
                                              BIT_14 | BIT_13 | BIT_12 | BIT_11 | BIT_10);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8647);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0xE600);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8036);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8078);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x3000);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x89E9);
        rtl8126_set_eth_phy_ocp_bit(tp, 0xB87E, 0xFF00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFD);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFE);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0200);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8FFF);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0400);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8018);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x7700);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8F9C);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0005);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x00ED);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0502);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0B00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0xD401);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8FA8);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xA438,
                                              0xFF00,
                                              0x2900);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814B);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x814F);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0B00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8142);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8144);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8150);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8118);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0700);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811A);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0700);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811C);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0500);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x810F);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8111);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x811D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0100);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD1C, BIT_8);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xADE8,
                                              BIT_15 | BIT_14 | BIT_13 | BIT_12 | BIT_11 | BIT_10 | BIT_9 | BIT_8 | BIT_7 | BIT_6,
                                              BIT_12 | BIT_10);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x864B);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x9D00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x862C);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x1200);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA436, 0x8566);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x003F);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x3F02);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x023C);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x3B0A);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x1C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xA438, 0x0000);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xAD9C, BIT_5);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8122);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x82C8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0009);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000B);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0021);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F7);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03B8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0049);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0049);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03B8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F7);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0021);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000B);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0009);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FF);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);

        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x80EF);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x0C00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x82A0);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000E);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03FE);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03ED);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0006);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x001A);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F1);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03D8);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0023);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0054);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0322);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x00DD);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03AB);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03DC);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0027);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x000E);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03E5);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F9);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0012);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x0001);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87E, 0x03F1);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xA430, BIT_1 | BIT_0);

        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB54C,
                                              0xFFC0,
                                              0x3700);

        rtl8126_set_eth_phy_ocp_bit(tp, 0xB648, BIT_6);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x8082);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x5D00);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x807C);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x5000);
        rtl8126_mdio_direct_write_phy_ocp(tp, 0xB87C, 0x809D);
        rtl8126_clear_and_set_eth_phy_ocp_bit(tp,
                                              0xB87E,
                                              0xFF00,
                                              0x5000);

        if (aspm && HW_HAS_WRITE_PHY_MCU_RAM_CODE(tp))
                rtl8126_enable_phy_aldps(tp);
}

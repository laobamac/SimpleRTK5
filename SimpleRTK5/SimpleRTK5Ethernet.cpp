//
//  SimpleRTK5Ethernet.cpp
//  SimpleRTK5
//
//  Created by laobamac on 2025/10/7.
//

/* LucyRTL8125Ethernet.cpp -- RTL8125 driver class implementation.
*
* Copyright (c) 2020 Laura Müller <laura-mueller@uni-duesseldorf.de>
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the Free
* Software Foundation; either version 2 of the License, or (at your option)
* any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* Driver for Realtek RTL8125 PCIe 2.5GB ethernet controllers.
*
* This driver is based on Realtek's r8125 Linux driver (9.003.04).
*/

#include "SimpleRTK5Ethernet.hpp"

#pragma mark --- function prototypes ---

static inline void prepareTSO4(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss);
static inline void prepareTSO6(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss);
static inline u32 ether_crc(int length, unsigned char *data);

#pragma mark --- public methods ---

OSDefineMetaClassAndStructors(SimpleRTK5, super)

bool SimpleRTK5::init(OSDictionary *properties)
{
    if (!super::init(properties))
        return false;

    // 初始化成员变量
    workLoop = NULL;
    commandGate = NULL;
    pciDevice = NULL;
    mediumDict = NULL;
    txQueue = NULL;
    interruptSource = NULL;
    timerSource = NULL;
    netif = NULL;
    netStats = NULL;
    etherStats = NULL;
    baseMap = NULL;
    baseAddr = NULL;
    rxBufferSize = kRxBufferSize4K;
    rxMbufCursor = NULL;
    sparePktHead = NULL;
    sparePktTail = NULL;
    spareNum = 0;
    txMbufCursor = NULL;
    rxBufArrayMem = NULL;
    txBufArrayMem = NULL;
    statBufDesc = NULL;
    statPhyAddr = (IOPhysicalAddress64)NULL;
    statData = NULL;
    stateFlags = 0;
    mtu = ETH_DATA_LEN;
    powerState = 0;
    speed = 0;
    duplex = DUPLEX_FULL;
    autoneg = AUTONEG_ENABLE;
    flowCtl = kFlowControlOff;
    eeeCap = 0;
    
    // 初始化Linux兼容层数据结构
    linuxData.configASPM = 0;
    linuxData.configEEE = 0;
    linuxData.s0MagicPacket = 0;
    linuxData.hwoptimize = 0;
    pciDeviceData.vendor = 0;
    pciDeviceData.device = 0;
    pciDeviceData.subsystem_vendor = 0;
    pciDeviceData.subsystem_device = 0;
    linuxData.pci_dev = &pciDeviceData;
    pollInterval2500 = 0;
    wolCapable = false;
    wolActive = false;
    enableTSO4 = false;
    enableTSO6 = false;
    enableCSO6 = false;
    pciPMCtrlOffset = 0;
    memset(fallBackMacAddr.bytes, 0, kIOEthernetAddressSize);
    
#ifdef DEBUG
    lastRxIntrupts = lastTxIntrupts = lastTmrIntrupts = tmrInterrupts = 0;
#endif

    return true;
}

void SimpleRTK5::free()
{
    UInt32 i;
    
    // 清理工作循环和事件源
    if (workLoop) {
        if (interruptSource) {
            workLoop->removeEventSource(interruptSource);
            RELEASE(interruptSource);
        }
        if (timerSource) {
            workLoop->removeEventSource(timerSource);
            RELEASE(timerSource);
        }
        workLoop->release();
        workLoop = NULL;
    }
    
    RELEASE(commandGate);
    RELEASE(txQueue);
    RELEASE(mediumDict);
    
    for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
        mediumTable[i] = NULL;
    
    RELEASE(baseMap);
    baseAddr = NULL;
    linuxData.mmio_addr = NULL;
    RELEASE(pciDevice);
    
    // 释放DMA资源
    freeTxResources();
    freeRxResources();
    freeStatResources();
    
    super::free();
}

bool SimpleRTK5::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    clear_mask((__M_CAST_M | __PROMISC_M), &stateFlags);
    multicastFilter = 0;

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) return false;
    
    pciDevice->retain();
    if (!pciDevice->open(this)) {
        pciDevice->release();
        pciDevice = NULL;
        return false;
    }
    
    mapper = IOMapper::copyMapperForDevice(pciDevice);
    getParams();
    
    // 初始化硬件配置
    if (!initPCIConfigSpace(pciDevice) || !initRTL8126()) {
        IOLog("SimpleRTK5: Hardware init failed.\n");
        goto error_cfg;
    }
    
    if (!setupMediumDict()) goto error_cfg;
    
    commandGate = getCommandGate();
    if (!commandGate) goto error_gate;
    commandGate->retain();
    
    // 分配环形缓冲区资源
    if (!setupTxResources() || !setupRxResources() || !setupStatResources()) {
        IOLog("SimpleRTK5: Resource allocation failed.\n");
        goto error_src;
    }

    if (!initEventSources(provider)) goto error_src;
    
    if (!attachInterface(reinterpret_cast<IONetworkInterface**>(&netif))) {
        IOLog("SimpleRTK5: attachInterface failed.\n");
        goto error_src;
    }

    pciDevice->close(this);
    return true;
    
done:
    return true;

error_src:
    freeStatResources();
    freeRxResources();
    freeTxResources();
    RELEASE(commandGate);
error_gate:
    RELEASE(mediumDict);
error_cfg:
    pciDevice->close(this);
    pciDevice->release();
    pciDevice = NULL;
    return false;
}

void SimpleRTK5::stop(IOService *provider)
{
    UInt32 i;
    
    if (netif) {
        detachInterface(netif);
        netif = NULL;
    }
    
    if (workLoop) {
        if (interruptSource) {
            workLoop->removeEventSource(interruptSource);
            RELEASE(interruptSource);
        }
        if (timerSource) {
            workLoop->removeEventSource(timerSource);
            RELEASE(timerSource);
        }
        workLoop->release();
        workLoop = NULL;
    }
    
    RELEASE(commandGate);
    RELEASE(txQueue);
    RELEASE(mediumDict);
    
    for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
        mediumTable[i] = NULL;

    freeStatResources();
    freeRxResources();
    freeTxResources();

    RELEASE(baseMap);
    baseAddr = NULL;
    linuxData.mmio_addr = NULL;
    RELEASE(pciDevice);
    
    super::stop(provider);
}

// 电源管理配置
static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

IOReturn SimpleRTK5::registerWithPolicyMaker(IOService *policyMaker)
{
    powerState = kPowerStateOn;
    return policyMaker->registerPowerDriver(this, powerStateArray, kPowerStateCount);
}

IOReturn SimpleRTK5::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    if (powerStateOrdinal == powerState) return IOPMAckImplied;
    
    if (powerStateOrdinal == kPowerStateOff)
        commandGate->runAction(setPowerStateSleepAction);
    else
        commandGate->runAction(setPowerStateWakeAction);

    powerState = powerStateOrdinal;
    return IOPMAckImplied;
}

void SimpleRTK5::systemWillShutdown(IOOptionBits specifier)
{
    if ((kIOMessageSystemWillPowerOff | kIOMessageSystemWillRestart) & specifier) {
        disable(netif);
        // 恢复原始MAC地址
        rtl8126_rar_set(&linuxData, (UInt8 *)&origMacAddr.bytes);
    }
    super::systemWillShutdown(specifier);
}

IOReturn SimpleRTK5::enable(IONetworkInterface *netif)
{
    const IONetworkMedium *selectedMedium;
    
    if (test_bit(__ENABLED, &stateFlags)) return kIOReturnSuccess;
    
    if (!pciDevice || pciDevice->isOpen()) return kIOReturnError;
    pciDevice->open(this);
    
    selectedMedium = getSelectedMedium();
    if (!selectedMedium) selectedMedium = mediumTable[MEDIUM_INDEX_AUTO];
    
    selectMedium(selectedMedium);
    enableRTL8126();
    interruptSource->enable();

    txDescDoneCount = txDescDoneLast = 0;
    deadlockWarn = 0;
    needsUpdate = false;
    set_bit(__ENABLED, &stateFlags);
    clear_bit(__POLL_MODE, &stateFlags);

    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::disable(IONetworkInterface *netif)
{
    UInt64 timeout, delay, now, t;

    if (!test_bit(__ENABLED, &stateFlags)) return kIOReturnSuccess;
    
    netif->stopOutputThread();
    netif->flushOutputQueue();
    
    // 等待轮询模式退出
    if (test_bit(__POLLING, &stateFlags)) {
        nanoseconds_to_absolutetime(5000, &delay);
        clock_get_uptime(&now);
        timeout = delay * 10;
        t = delay;

        while (test_bit(__POLLING, &stateFlags) && (t < timeout)) {
            clock_delay_until(now + t);
            t += delay;
        }
    }
    
    clear_mask((__ENABLED_M | __LINK_UP_M | __POLL_MODE_M | __POLLING_M), &stateFlags);
    timerSource->cancelTimeout();
    needsUpdate = false;
    txDescDoneCount = txDescDoneLast = 0;

    interruptSource->disable();
    disableRTL8126();
    clearRxTxRings();
    
    if (pciDevice && pciDevice->isOpen())
        pciDevice->close(this);
        
    return kIOReturnSuccess;
}

// 核心发包逻辑
IOReturn SimpleRTK5::outputStart(IONetworkInterface *interface, IOOptionBits options )
{
    IOPhysicalSegment txSegments[kMaxSegs];
    mbuf_t m;
    RtlTxDesc *desc, *firstDesc;
    UInt32 cmd, opts2, offloadFlags, mss, len, tcpOff, opts1, vlanTag;
    UInt32 numSegs, lastSeg, index, i;
    struct rtl8126_private *tp = &linuxData;
    
    if (!(test_mask((__ENABLED_M | __LINK_UP_M), &stateFlags))) {
        // 链路未连接，清理队列
        while (interface->dequeueOutputPackets(1, &m, NULL, NULL, NULL) == kIOReturnSuccess) {
            freePacket(m);
        }
        return kIOReturnOutputStall;
    }

    // 循环处理发送队列
    while ((txNumFreeDesc > (kMaxSegs + 3)) && (interface->dequeueOutputPackets(1, &m, NULL, NULL, NULL) == kIOReturnSuccess)) {
        cmd = 0;
        opts2 = 0;
        len = (UInt32)mbuf_pkthdr_len(m);

        if (mbuf_get_tso_requested(m, &offloadFlags, &mss)) {
            freePacket(m);
            continue;
        }

        // 处理硬件卸载 (TSO/CSUM)
        if (offloadFlags & (MBUF_TSO_IPV4 | MBUF_TSO_IPV6)) {
            if (offloadFlags & MBUF_TSO_IPV4) {
                if ((len - kMacHdrLen) > mtu) {
                    prepareTSO4(m, &tcpOff, &mss);
                    cmd = (GiantSendv4 | (tcpOff << GTTCPHO_SHIFT));
                    opts2 = ((mss & MSSMask) << MSSShift_8125);
                } else {
                    offloadFlags = kChecksumTCP;
                    opts2 = (TxIPCS_C | TxTCPCS_C);
                }
            } else {
                if ((len - kMacHdrLen) > mtu) {
                    prepareTSO6(m, &tcpOff, &mss);
                    cmd = (GiantSendv6 | (tcpOff << GTTCPHO_SHIFT));
                    opts2 = ((mss & MSSMask) << MSSShift_8125);
                } else {
                    offloadFlags = kChecksumTCPIPv6;
                    opts2 = (TxTCPCS_C | TxIPV6F_C | (((kMacHdrLen + kIPv6HdrLen) & TCPHO_MAX) << TCPHO_SHIFT));
                }
            }
        } else {
            mbuf_get_csum_requested(m, &offloadFlags, &mss);
            if (offloadFlags & kChecksumTCP) opts2 = (TxIPCS_C | TxTCPCS_C);
            else if (offloadFlags & kChecksumTCPIPv6) opts2 = (TxTCPCS_C | TxIPV6F_C | (((kMacHdrLen + kIPv6HdrLen) & TCPHO_MAX) << TCPHO_SHIFT));
            else if (offloadFlags & kChecksumUDP) opts2 = (TxIPCS_C | TxUDPCS_C);
            else if (offloadFlags & kChecksumUDPIPv6) opts2 = (TxUDPCS_C | TxIPV6F_C | (((kMacHdrLen + kIPv6HdrLen) & TCPHO_MAX) << TCPHO_SHIFT));
            else if (offloadFlags & kChecksumIP) opts2 = TxIPCS_C;
        }

        // 获取物理地址片段
        numSegs = txMbufCursor->getPhysicalSegmentsWithCoalesce(m, &txSegments[0], kMaxSegs);
        if (!numSegs) {
            freePacket(m);
            continue;
        }

        // 更新环形缓冲区索引
        OSAddAtomic(-numSegs, &txNumFreeDesc);
        index = txNextDescIndex;
        txNextDescIndex = (txNextDescIndex + numSegs) & kTxDescMask;
        txTailPtr0 += numSegs;
        firstDesc = &txDescArray[index];
        lastSeg = numSegs - 1;

        opts2 |= (getVlanTagDemand(m, &vlanTag)) ? (OSSwapInt16(vlanTag) | TxVlanTag) : 0;
        
        // 填充描述符
        for (i = 0; i < numSegs; i++) {
            desc = &txDescArray[index];
            opts1 = (((UInt32)txSegments[i].length) | cmd);
            opts1 |= (i == 0) ? FirstFrag : DescOwn;
            
            if (i == lastSeg) {
                opts1 |= LastFrag;
                txMbufArray[index] = m;
            } else {
                txMbufArray[index] = NULL;
            }
            if (index == kTxLastDesc)
                opts1 |= RingEnd;
            
            desc->addr = OSSwapHostToLittleInt64(txSegments[i].location);
            desc->opts2 = OSSwapHostToLittleInt32(opts2);
            desc->opts1 = OSSwapHostToLittleInt32(opts1);
            
            ++index &= kTxDescMask;
        }
        firstDesc->opts1 |= DescOwn;
    }
    
    // 写入尾指针触发发送
    WriteReg32(SW_TAIL_PTR0_8126, txTailPtr0 & tp->MaxTxDescPtrMask);

    return (txNumFreeDesc > (kMaxSegs + 3)) ? kIOReturnSuccess : kIOReturnNoResources;
}

void SimpleRTK5::getPacketBufferConstraints(IOPacketBufferConstraints *constraints) const
{
    constraints->alignStart = kIOPacketBufferAlign1;
    constraints->alignLength = kIOPacketBufferAlign1;
}

IOOutputQueue* SimpleRTK5::createOutputQueue()
{
    return IOBasicOutputQueue::withTarget(this);
}

const OSString* SimpleRTK5::newVendorString() const
{
    return OSString::withCString("Realtek");
}

const OSString* SimpleRTK5::newModelString() const
{
    return OSString::withCString(rtl_chip_info[linuxData.chipset].name);
}

bool SimpleRTK5::configureInterface(IONetworkInterface *interface)
{
    char modelName[kNameLenght];
    IONetworkData *data;
    bool result = super::configureInterface(interface);
    
    if (!result) return false;
    
    data = interface->getParameter(kIONetworkStatsKey);
    if (data) netStats = (IONetworkStats *)data->getBuffer();
    
    data = interface->getParameter(kIOEthernetStatsKey);
    if (data) etherStats = (IOEthernetStats *)data->getBuffer();
    
    if (!netStats || !etherStats) return false;

    if (interface->configureOutputPullModel((kNumTxDesc/2), 0, 0, IONetworkInterface::kOutputPacketSchedulingModelNormal) != kIOReturnSuccess)
        return false;
    
    if (interface->configureInputPacketPolling(kNumRxDesc, 0) != kIOReturnSuccess)
        return false;
    
    snprintf(modelName, kNameLenght, "Realtek %s PCIe 2.5/5 Gbit Ethernet", rtl_chip_info[linuxData.chipset].name);
    setProperty("model", modelName);
    
    return true;
}

bool SimpleRTK5::createWorkLoop()
{
    workLoop = IOWorkLoop::workLoop();
    return (workLoop != NULL);
}

IOWorkLoop* SimpleRTK5::getWorkLoop() const
{
    return workLoop;
}

IOReturn SimpleRTK5::getHardwareAddress(IOEthernetAddress *addr)
{
    if (addr) {
        bcopy(&currMacAddr.bytes, addr->bytes, kIOEthernetAddressSize);
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}

IOReturn SimpleRTK5::setPromiscuousMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;

    if (active) {
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys);
        mcFilter[1] = mcFilter[0] = 0xffffffff;
        set_bit(__PROMISC, &stateFlags);
    } else {
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
        clear_bit(__PROMISC, &stateFlags);
    }
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR0 + 4, mcFilter[1]);

    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMulticastMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;

    if (active) {
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
        set_bit(__M_CAST, &stateFlags);
    } else{
        rxMode = (AcceptBroadcast | AcceptMyPhys);
        mcFilter[1] = mcFilter[0] = 0;
        clear_bit(__M_CAST, &stateFlags);
    }
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR0 + 4, mcFilter[1]);
    
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt64 filter = 0;
    UInt32 i, bitNumber;
    
    if (count <= kMCFilterLimit) {
        for (i = 0; i < count; i++, addrs++) {
            bitNumber = ether_crc(6, reinterpret_cast<unsigned char *>(addrs)) >> 26;
            filter |= (1 << (bitNumber & 0x3f));
        }
        multicastFilter = OSSwapInt64(filter);
    } else {
        multicastFilter = 0xffffffffffffffff;
    }
    WriteReg32(MAR0, *filterAddr++);
    WriteReg32(MAR0 + 4, *filterAddr);

    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput)
{
    if ((checksumFamily == kChecksumFamilyTCPIP) && checksumMask) {
        if (isOutput) {
            *checksumMask = (enableCSO6) ? (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6) : (kChecksumTCP | kChecksumUDP | kChecksumIP);
        } else {
            *checksumMask = (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6);
        }
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

UInt32 SimpleRTK5::getFeatures() const
{
    UInt32 features = (kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan);
    
    if (enableTSO4) features |= kIONetworkFeatureTSOIPv4;
    if (enableTSO6) features |= kIONetworkFeatureTSOIPv6;
    
    return features;
}

IOReturn SimpleRTK5::setWakeOnMagicPacket(bool active)
{
    if (wolCapable) {
        linuxData.wol_enabled = active ? WOL_ENABLED : WOL_DISABLED;
        wolActive = active;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn SimpleRTK5::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if ((group == gIOEthernetWakeOnLANFilterGroup) && wolCapable) {
        *filters = kIOEthernetWakeOnMagicPacket;
        return kIOReturnSuccess;
    }
    return super::getPacketFilters(group, filters);
}

IOReturn SimpleRTK5::setHardwareAddress(const IOEthernetAddress *addr)
{
    if (addr) {
        bcopy(addr->bytes, &currMacAddr.bytes, kIOEthernetAddressSize);
        rtl8126_rar_set(&linuxData, (UInt8 *)&currMacAddr.bytes);
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}

IOReturn SimpleRTK5::selectMedium(const IONetworkMedium *medium)
{
    if (!medium) return kIOReturnSuccess;

    autoneg = AUTONEG_DISABLE;
    flowCtl = kFlowControlOff;
    linuxData.eee_adv_t = 0;
        
    switch (medium->getIndex()) {
        case MEDIUM_INDEX_AUTO:
            autoneg = AUTONEG_ENABLE;
            speed = 0;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_10HD:
            speed = SPEED_10;
            duplex = DUPLEX_HALF;
            break;
        case MEDIUM_INDEX_10FD:
            speed = SPEED_10;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_100HD:
            speed = SPEED_100;
            duplex = DUPLEX_HALF;
            break;
        case MEDIUM_INDEX_100FD:
            speed = SPEED_100;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_100FDFC:
            speed = SPEED_100;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            break;
        case MEDIUM_INDEX_1000FD:
            speed = SPEED_1000;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_1000FDFC:
            speed = SPEED_1000;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            break;
        case MEDIUM_INDEX_100FDEEE:
            speed = SPEED_100;
            duplex = DUPLEX_FULL;
            linuxData.eee_adv_t = eeeCap;
            break;
        case MEDIUM_INDEX_100FDFCEEE:
            speed = SPEED_100;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            linuxData.eee_adv_t = eeeCap;
            break;
        case MEDIUM_INDEX_1000FDEEE:
            speed = SPEED_1000;
            duplex = DUPLEX_FULL;
            linuxData.eee_adv_t = eeeCap;
            break;
        case MEDIUM_INDEX_1000FDFCEEE:
            speed = SPEED_1000;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            linuxData.eee_adv_t = eeeCap;
            break;
        case MEDIUM_INDEX_2500FD:
            speed = SPEED_2500;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_2500FDFC:
            speed = SPEED_2500;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            break;
        case MEDIUM_INDEX_5000FD:
            speed = SPEED_5000;
            duplex = DUPLEX_FULL;
            break;
        case MEDIUM_INDEX_5000FDFC:
            speed = SPEED_5000;
            duplex = DUPLEX_FULL;
            flowCtl = kFlowControlOn;
            break;
    }
    setCurrentMedium(medium);
    setLinkDown();
    return kIOReturnSuccess;
}

#pragma mark --- jumbo frame support methods ---

IOReturn SimpleRTK5::getMaxPacketSize(UInt32 * maxSize) const
{
    // macOS Ventura+ 允许真实巨帧支持
    if (version_major >= 22) {
        *maxSize = rxBufferSize - 2;
    } else {
        *maxSize = kMaxPacketSize;
    }
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMaxPacketSize(UInt32 maxSize)
{
    ifnet_t ifnet = netif->getIfnet();
    ifnet_offload_t offload;
    UInt32 mask = 0;

    if (maxSize <= (rxBufferSize - 2)) {
        mtu = maxSize - (ETH_HLEN + ETH_FCS_LEN);
        
        if (enableTSO4) mask |= IFNET_TSO_IPV4;
        if (enableTSO6) mask |= IFNET_TSO_IPV6;
        if (enableCSO6) mask |= (IFNET_CSUM_TCPIPV6 | IFNET_CSUM_UDPIPV6);

        offload = ifnet_offload(ifnet);
        
        // 巨帧时根据MSS调整硬件卸载
        if (mtu > MSS_MAX) {
            offload &= ~mask;
        } else {
            offload |= mask;
        }
        
        if (ifnet_set_offload(ifnet, offload)) {
            IOLog("SimpleRTK5: Error setting hardware offload: %x!\n", offload);
        }

        setLinkDown();
        timerSource->cancelTimeout();
        restartRTL8126();
        
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}

#pragma mark --- common interrupt methods ---

void SimpleRTK5::pciErrorInterrupt()
{
    UInt16 cmdReg = pciDevice->configRead16(kIOPCIConfigCommand);
    UInt16 statusReg = pciDevice->configRead16(kIOPCIConfigStatus);
    
    cmdReg |= (kIOPCICommandSERR | kIOPCICommandParityError);
    statusReg &= (kIOPCIStatusParityErrActive | kIOPCIStatusSERRActive | kIOPCIStatusMasterAbortActive | kIOPCIStatusTargetAbortActive | kIOPCIStatusTargetAbortCapable);
    pciDevice->configWrite16(kIOPCIConfigCommand, cmdReg);
    pciDevice->configWrite16(kIOPCIConfigStatus, statusReg);
    
    restartRTL8126();
}

void SimpleRTK5::txInterrupt()
{
    mbuf_t m;
    UInt32 nextClosePtr = ReadReg32(HW_CLO_PTR0_8126);
    UInt32 oldDirtyIndex = txDirtyDescIndex;
    UInt32 numDone;
    struct rtl8126_private *tp = &linuxData;

    numDone = (nextClosePtr - txClosePtr0) & tp->MaxTxDescPtrMask;
    txClosePtr0 = nextClosePtr;
    
    // 清理已发送的数据包描述符
    while (numDone-- > 0) {
        m = txMbufArray[txDirtyDescIndex];
        txMbufArray[txDirtyDescIndex] = NULL;

        if (m) freePacket(m, kDelayFree);

        txDescDoneCount++;
        OSIncrementAtomic(&txNumFreeDesc);
        ++txDirtyDescIndex &= kTxDescMask;
    }
    
    if (oldDirtyIndex != txDirtyDescIndex) {
        if (txNumFreeDesc > kTxQueueWakeTreshhold)
            netif->signalOutputThread();
        
        releaseFreePackets();
    }
}

// 核心收包逻辑
UInt32 SimpleRTK5::rxInterrupt(IONetworkInterface *interface, uint32_t maxCount, IOMbufQueue *pollQueue, void *context)
{
    IOPhysicalSegment rxSegment;
    RtlRxDesc *desc = &rxDescArray[rxNextDescIndex];
    mbuf_t bufPkt, newPkt;
    UInt64 addr;
    UInt32 opts1, opts2, descStatus1, descStatus2, pktSize;
    UInt32 goodPkts = 0;
    bool replaced;
    
    while (!((descStatus1 = OSSwapLittleToHostInt32(desc->opts1)) & DescOwn) && (goodPkts < maxCount)) {
        opts1 = (rxNextDescIndex == kRxLastDesc) ? (RingEnd | DescOwn) : DescOwn;
        opts2 = 0;
        addr = 0;
        
        // 检查分片和错误
        if (unlikely((descStatus1 & (FirstFrag|LastFrag)) != (FirstFrag|LastFrag))) {
            etherStats->dot3StatsEntry.frameTooLongs++;
            opts1 |= rxBufferSize;
            goto nextDesc;
        }
        
        if (unlikely(descStatus1 & RxRES)) {
            if (descStatus1 & (RxRWT | RxRUNT))
                etherStats->dot3StatsEntry.frameTooLongs++;
            if (descStatus1 & RxCRC)
                etherStats->dot3StatsEntry.fcsErrors++;
            opts1 |= rxBufferSize;
            goto nextDesc;
        }
        
        descStatus2 = OSSwapLittleToHostInt32(desc->opts2);
        pktSize = (descStatus1 & 0x1fff) - kIOEthernetCRCSize;
        bufPkt = rxMbufArray[rxNextDescIndex];
        
        // 尝试替换mbuf
        newPkt = replaceOrCopyPacket(&bufPkt, pktSize, &replaced);
        
        if (unlikely(!newPkt)) {
            // 分配失败，尝试使用备用包
            if (spareNum > 1) {
                OSDecrementAtomic(&spareNum);
                newPkt = bufPkt;
                replaced = true;
                bufPkt = sparePktHead;
                sparePktHead = mbuf_next(sparePktHead);
                mbuf_setnext(bufPkt, NULL);
                goto handle_pkt;
            }
            etherStats->dot3RxExtraEntry.resourceErrors++;
            opts1 |= rxBufferSize;
            goto nextDesc;
        }

    handle_pkt:
        if (replaced) {
            if (rxMbufCursor->getPhysicalSegments(bufPkt, &rxSegment, 1) != 1) {
                etherStats->dot3RxExtraEntry.resourceErrors++;
                freePacket(bufPkt);
                opts1 |= rxBufferSize;
                goto nextDesc;
            }
            opts1 |= ((UInt32)rxSegment.length & 0x0000ffff);
            addr = rxSegment.location;
            rxMbufArray[rxNextDescIndex] = bufPkt;
        } else {
            opts1 |= rxBufferSize;
        }

        mbuf_setlen(newPkt, pktSize);
        getChecksumResult(newPkt, descStatus1, descStatus2);

        if (descStatus2 & RxVlanTag)
            setVlanTag(newPkt, OSSwapInt16(descStatus2 & 0xffff));

        mbuf_pkthdr_setlen(newPkt, pktSize);
        interface->enqueueInputPacket(newPkt, pollQueue);
        goodPkts++;
        
    nextDesc:
        if (addr) desc->addr = OSSwapHostToLittleInt64(addr);
        desc->opts2 = OSSwapHostToLittleInt32(opts2);
        desc->opts1 = OSSwapHostToLittleInt32(opts1);
        
        ++rxNextDescIndex &= kRxDescMask;
        desc = &rxDescArray[rxNextDescIndex];
    }
    return goodPkts;
}

void SimpleRTK5::checkLinkStatus()
{
    struct rtl8126_private *tp = &linuxData;
    UInt16 currLinkState;
    
    currLinkState = ReadReg32(PHYstatus);
    
    if (currLinkState & LinkStatus) {
        eeeMode = getEEEMode();
        flowCtl = (currLinkState & (TxFlowCtrl | RxFlowCtrl)) ? kFlowControlOn : kFlowControlOff;

        if (currLinkState & _5000bpsF) {
            speed = SPEED_5000;
            duplex = DUPLEX_FULL;
        } else if (currLinkState & _2500bpsF) {
            speed = SPEED_2500;
            duplex = DUPLEX_FULL;
        } else if (currLinkState & _1000bpsF) {
            speed = SPEED_1000;
            duplex = DUPLEX_FULL;
        } else if (currLinkState & _100bps) {
            speed = SPEED_100;
            duplex = (currLinkState & FullDup) ? DUPLEX_FULL : DUPLEX_HALF;
        } else {
            speed = SPEED_10;
            duplex = (currLinkState & FullDup) ? DUPLEX_FULL : DUPLEX_HALF;
        }
        setupRTL8126();

        switch (tp->mcfg) {
            case CFG_METHOD_1:
            case CFG_METHOD_2:
            case CFG_METHOD_3:
                if (RTL_R8(tp, PHYstatus) & _10bps) rtl8126_enable_eee_plus(tp);
                break;
            default: break;
        }

        setLinkUp();
        timerSource->setTimeoutMS(kTimeoutMS);
        
        rtl8126_mdio_write(tp, 0x1F, 0x0000);
        linuxData.phy_reg_anlpar = rtl8126_mdio_read(tp, MII_LPA);
    } else {
        tp->phy_reg_anlpar = 0;
        rtl8126_disable_eee_plus(tp);
        timerSource->cancelTimeout();
        setLinkDown();
    }
}

void SimpleRTK5::interruptHandler(OSObject *client, IOInterruptEventSource *src, int count)
{
    UInt32 packets;
    UInt32 status;
    
    status = ReadReg32(ISR0_8125);
    
    if ((status == 0xFFFFFFFF) || !status) return;
    
    WriteReg32(IMR0_8125, 0x0000);
    WriteReg32(ISR0_8125, (status & ~RxFIFOOver));

    if (status & SYSErr) {
        pciErrorInterrupt();
        goto done;
    }

    if (!test_bit(__POLL_MODE, &stateFlags) && !test_and_set_bit(__POLLING, &stateFlags)) {
        if (status & (RxOK | RxDescUnavail | RxFIFOOver)) {
            packets = rxInterrupt(netif, kNumRxDesc, NULL, NULL);
            if (packets) netif->flushInputQueue();
            etherStats->dot3RxExtraEntry.interrupts++;
            if (spareNum < kRxNumSpareMbufs) refillSpareBuffers();
        }
        
        if (status & (TxOK | RxOK | PCSTimeout)) {
            txInterrupt();
            if (status & TxOK) etherStats->dot3TxExtraEntry.interrupts++;
        }
        
        if (status & (TxOK | RxOK)) {
            WriteReg32(TIMER_INT0_8125, 0x2600);
            WriteReg32(TCTR0_8125, 0x2600);
            intrMask = intrMaskTimer;
        } else if (status & PCSTimeout) {
            WriteReg32(TIMER_INT0_8125, 0x0000);
            intrMask = intrMaskRxTx;
        }
        
        clear_bit(__POLLING, &stateFlags);
    }
    
    if (status & LinkChg) {
        checkLinkStatus();
        WriteReg32(TIMER_INT0_8125, 0x000);
        intrMask = intrMaskRxTx;
    }

done:
    WriteReg32(IMR0_8125, intrMask);
}

bool SimpleRTK5::txHangCheck()
{
    if ((txDescDoneCount == txDescDoneLast) && (txNumFreeDesc < kNumTxDesc)) {
        if (++deadlockWarn == kTxCheckTreshhold) {
            etherStats->dot3TxExtraEntry.timeouts++;
            txInterrupt();
        } else if (deadlockWarn >= kTxDeadlockTreshhold) {
            IOLog("SimpleRTK5: Tx stalled. Resetting chipset.\n");
            etherStats->dot3TxExtraEntry.resets++;
            restartRTL8126();
            return true;
        }
    } else {
        deadlockWarn = 0;
    }
    return false;
}

#pragma mark --- rx poll methods ---

IOReturn SimpleRTK5::setInputPacketPollingEnable(IONetworkInterface *interface, bool enabled)
{
    if (test_bit(__ENABLED, &stateFlags)) {
        if (enabled) {
            set_bit(__POLL_MODE, &stateFlags);
            intrMask = intrMaskPoll;
        } else {
            clear_bit(__POLL_MODE, &stateFlags);
            intrMask = intrMaskRxTx;
        }
        WriteReg32(IMR0_8125, intrMask);
    }
    return kIOReturnSuccess;
}

void SimpleRTK5::pollInputPackets(IONetworkInterface *interface, uint32_t maxCount, IOMbufQueue *pollQueue, void *context )
{
    if (test_bit(__POLL_MODE, &stateFlags) && !test_and_set_bit(__POLLING, &stateFlags)) {
        rxInterrupt(interface, maxCount, pollQueue, context);
        txInterrupt();
        clear_bit(__POLLING, &stateFlags);
        
        if (spareNum < kRxNumSpareMbufs)
            commandGate->runAction(refillAction);
    }
}

#pragma mark --- hardware specific methods ---

inline void SimpleRTK5::getChecksumResult(mbuf_t m, UInt32 status1, UInt32 status2)
{
    mbuf_csum_performed_flags_t performed = 0;
    UInt32 value = 0;

    if ((status2 & RxV4F) && !(status1 & RxIPF))
        performed |= (MBUF_CSUM_DID_IP | MBUF_CSUM_IP_GOOD);

    if (((status1 & RxTCPT) && !(status1 & RxTCPF)) ||
        ((status1 & RxUDPT) && !(status1 & RxUDPF))) {
        performed |= (MBUF_CSUM_DID_DATA | MBUF_CSUM_PSEUDO_HDR);
        value = 0xffff;
    }
    if (performed)
        mbuf_set_csum_performed(m, performed, value);
}

static const char *speed5GName = "5 Gigabit";
static const char *speed25GName = "2.5 Gigabit";
static const char *speed1GName = "1 Gigabit";
static const char *speed100MName = "100 Megabit";
static const char *speed10MName = "10 Megabit";
static const char *duplexFullName = "full-duplex";
static const char *duplexHalfName = "half-duplex";
static const char *offFlowName = "no flow-control";
static const char *onFlowName = "flow-control";

static const char* eeeNames[kEEETypeCount] = {
    "",
    ", energy-efficient-ethernet"
};

void SimpleRTK5::setLinkUp()
{
    IONetworkPacketPollingParameters pParams;
    UInt64 mediumSpeed;
    UInt32 mediumIndex = MEDIUM_INDEX_AUTO;
    const char *speedName;
    const char *duplexName;
    const char *flowName;
    const char *eeeName;
    
    eeeName = eeeNames[kEEETypeNo];
    flowName = (flowCtl == kFlowControlOn) ? onFlowName : offFlowName;
    
    // 根据速度和双工模式选择对应的Medium索引
    if (speed == SPEED_5000) {
        mediumSpeed = kSpeed5000MBit;
        speedName = speed5GName;
        duplexName = duplexFullName;
        mediumIndex = (flowCtl == kFlowControlOn) ? MEDIUM_INDEX_5000FDFC : MEDIUM_INDEX_5000FD;
    } else if (speed == SPEED_2500) {
        mediumSpeed = kSpeed2500MBit;
        speedName = speed25GName;
        duplexName = duplexFullName;
        mediumIndex = (flowCtl == kFlowControlOn) ? MEDIUM_INDEX_2500FDFC : MEDIUM_INDEX_2500FD;
    } else if (speed == SPEED_1000) {
        mediumSpeed = kSpeed1000MBit;
        speedName = speed1GName;
        duplexName = duplexFullName;
        bool eee = (eeeMode & MDIO_EEE_1000T);
        if (eee) eeeName = eeeNames[kEEETypeYes];
        
        if (flowCtl == kFlowControlOn) mediumIndex = eee ? MEDIUM_INDEX_1000FDFCEEE : MEDIUM_INDEX_1000FDFC;
        else mediumIndex = eee ? MEDIUM_INDEX_1000FDEEE : MEDIUM_INDEX_1000FD;
    } else if (speed == SPEED_100) {
        mediumSpeed = kSpeed100MBit;
        speedName = speed100MName;
        if (duplex == DUPLEX_FULL) {
            duplexName = duplexFullName;
            bool eee = (eeeMode & MDIO_EEE_100TX);
            if (eee) eeeName = eeeNames[kEEETypeYes];
            if (flowCtl == kFlowControlOn) mediumIndex = eee ? MEDIUM_INDEX_100FDFCEEE : MEDIUM_INDEX_100FDFC;
            else mediumIndex = eee ? MEDIUM_INDEX_100FDEEE : MEDIUM_INDEX_100FD;
        } else {
            mediumIndex = MEDIUM_INDEX_100HD;
            duplexName = duplexHalfName;
        }
    } else {
        mediumSpeed = kSpeed10MBit;
        speedName = speed10MName;
        if (duplex == DUPLEX_FULL) {
            mediumIndex = MEDIUM_INDEX_10FD;
            duplexName = duplexFullName;
        } else {
            mediumIndex = MEDIUM_INDEX_10HD;
            duplexName = duplexHalfName;
        }
    }

    WriteReg8(ChipCmd, CmdTxEnb | CmdRxEnb);
    set_bit(__LINK_UP, &stateFlags);
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, mediumTable[mediumIndex], mediumSpeed, NULL);

    // 配置轮询参数
    bzero(&pParams, sizeof(IONetworkPacketPollingParameters));
    if (speed == SPEED_10) {
        pParams.lowThresholdPackets = 2;
        pParams.highThresholdPackets = 8;
        pParams.lowThresholdBytes = 0x400;
        pParams.highThresholdBytes = 0x1800;
        pParams.pollIntervalTime = 1000000;
    } else {
        pParams.lowThresholdPackets = 10;
        pParams.highThresholdPackets = 40;
        pParams.lowThresholdBytes = 0x1000;
        pParams.highThresholdBytes = 0x10000;
        if (speed >= SPEED_2500) pParams.pollIntervalTime = pollInterval2500;
        else if (speed == SPEED_1000) pParams.pollIntervalTime = 170000;
        else pParams.pollIntervalTime = 1000000;
    }
    netif->setPacketPollingParameters(&pParams, 0);
    netif->startOutputThread();
    
    IOLog("SimpleRTK5: Link up %s, %s, %s%s\n", speedName, duplexName, flowName, eeeName);
}

void SimpleRTK5::setLinkDown()
{
    deadlockWarn = 0;
    needsUpdate = false;

    netif->stopOutputThread();
    netif->flushOutputQueue();

    clear_mask((__LINK_UP_M | __POLL_MODE_M), &stateFlags);
    setLinkStatus(kIONetworkLinkValid);

    rtl8126_nic_reset(&linuxData);
    clearRxTxRings();
    setPhyMedium();
}

void SimpleRTK5::updateStatitics()
{
    UInt32 sgColl, mlColl, cmd;

    if (needsUpdate && !(ReadReg32(CounterAddrLow) & CounterDump)) {
        needsUpdate = false;
        netStats->inputPackets = OSSwapLittleToHostInt64(statData->rxPackets) & 0x00000000ffffffff;
        netStats->inputErrors = OSSwapLittleToHostInt32(statData->rxErrors);
        netStats->outputPackets = OSSwapLittleToHostInt64(statData->txPackets) & 0x00000000ffffffff;
        netStats->outputErrors = OSSwapLittleToHostInt32(statData->txErrors);
        
        sgColl = OSSwapLittleToHostInt32(statData->txOneCollision);
        mlColl = OSSwapLittleToHostInt32(statData->txMultiCollision);
        netStats->collisions = sgColl + mlColl;
        
        etherStats->dot3StatsEntry.singleCollisionFrames = sgColl;
        etherStats->dot3StatsEntry.multipleCollisionFrames = mlColl;
        etherStats->dot3StatsEntry.alignmentErrors = OSSwapLittleToHostInt16(statData->alignErrors);
        etherStats->dot3StatsEntry.missedFrames = OSSwapLittleToHostInt16(statData->rxMissed);
        etherStats->dot3TxExtraEntry.underruns = OSSwapLittleToHostInt16(statData->txUnderun);
    }

    if (test_bit(__LINK_UP, &stateFlags) && (ReadReg8(ChipCmd) & CmdRxEnb)) {
        WriteReg32(CounterAddrHigh, (statPhyAddr >> 32));
        cmd = (statPhyAddr & 0x00000000ffffffff);
        WriteReg32(CounterAddrLow, cmd);
        WriteReg32(CounterAddrLow, cmd | CounterDump);
        needsUpdate = true;
    }
}

void SimpleRTK5::timerActionRTL8126(IOTimerEventSource *timer)
{
    updateStatitics();

    if (test_bit(__LINK_UP, &stateFlags)) {
        if (!txHangCheck()) {
            timerSource->setTimeoutMS(kTimeoutMS);
        }
    }
    txDescDoneLast = txDescDoneCount;
}

#pragma mark --- miscellaneous functions ---

static inline void prepareTSO4(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss)
{
    UInt8 *p = (UInt8 *)mbuf_data(m) + kMacHdrLen;
    struct ip4_hdr_be *ip = (struct ip4_hdr_be *)p;
    struct tcp_hdr_be *tcp;
    UInt32 csum32 = 6;
    UInt32 i, il;
    
    for (i = 0; i < 4; i++) {
        csum32 += ntohs(ip->addr[i]);
        csum32 += (csum32 >> 16);
        csum32 &= 0xffff;
    }
    il = ((ip->hdr_len & 0x0f) << 2);
    
    tcp = (struct tcp_hdr_be *)(p + il);
    
    // 填充TSOv4伪首部校验和
    tcp->csum = htons((UInt16)csum32);
    *tcpOffset = kMacHdrLen + il;
    if (*mss > MSS_MAX) *mss = MSS_MAX;
}

static inline void prepareTSO6(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss)
{
    UInt8 *p = (UInt8 *)mbuf_data(m) + kMacHdrLen;
    struct ip6_hdr_be *ip6 = (struct ip6_hdr_be *)p;
    struct tcp_hdr_be *tcp;
    UInt32 csum32 = 6;
    UInt32 i;

    ip6->pay_len = 0;
    for (i = 0; i < 16; i++) {
        csum32 += ntohs(ip6->addr[i]);
        csum32 += (csum32 >> 16);
        csum32 &= 0xffff;
    }
    
    tcp = (struct tcp_hdr_be *)(p + kIPv6HdrLen);
    
    // 填充TSOv6伪首部校验和
    tcp->csum = htons((UInt16)csum32);
    *tcpOffset = kMacHdrLen + kIPv6HdrLen;
    if (*mss > MSS_MAX) *mss = MSS_MAX;
}

static unsigned const ethernet_polynomial = 0x04c11db7U;

static inline u32 ether_crc(int length, unsigned char *data)
{
    int crc = -1;
    while(--length >= 0) {
        unsigned char current_octet = *data++;
        for (int bit = 0; bit < 8; bit++, current_octet >>= 1) {
            crc = (crc << 1) ^ ((crc < 0) ^ (current_octet & 1) ? ethernet_polynomial : 0);
        }
    }
    return crc;
}

//
//  SimpleRTK5Ethernet.cpp
//  SimpleRTK5
//
//  Created by laobamac on 2025/10/7.
//

#include "SimpleRTK5Ethernet.hpp"

static inline void prepareTSO4(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss);
static inline void prepareTSO6(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss);

static inline u32 ether_crc(int length, unsigned char *data);

OSDefineMetaClassAndStructors(SimpleRTK5, super)

bool SimpleRTK5::init(OSDictionary *properties)
{
    bool result;
    
    result = super::init(properties);
    
    if (result) {
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

        /* 初始化状态标志 */
        stateFlags = 0;
        
        mtu = ETH_DATA_LEN;
        powerState = 0;
        speed = 0;
        duplex = DUPLEX_FULL;
        autoneg = AUTONEG_ENABLE;
        flowCtl = kFlowControlOff;
        eeeCap = 0;
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
    }
    
done:
    return result;
}

void SimpleRTK5::free()
{
    UInt32 i;
    
    DebugLog("SimpleRTK5: free() ===>\n");
    
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
    freeTxResources();
    freeRxResources();
    freeStatResources();
    
    DebugLog("SimpleRTK5: free() <===\n");
    
    super::free();
}

bool SimpleRTK5::start(IOService *provider)
{
    bool result;
    
    result = super::start(provider);
    
    if (!result) {
        IOLog("SimpleRTK5: IOEthernetController::start failed.\n");
        goto done;
    }
    clear_mask((__M_CAST_M | __PROMISC_M), &stateFlags);
    multicastFilter = 0;

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    
    if (!pciDevice) {
        IOLog("SimpleRTK5: No provider.\n");
        goto done;
    }
    pciDevice->retain();
    
    if (!pciDevice->open(this)) {
        IOLog("SimpleRTK5: Failed to open provider.\n");
        goto error_open;
    }
    mapper = IOMapper::copyMapperForDevice(pciDevice);

    getParams();
    
    /* 初始化PCI配置空间 */
    if (!initPCIConfigSpace(pciDevice)) {
        goto error_cfg;
    }

    /* 初始化芯片硬件 */
    if (!initRTL8126()) {
        IOLog("SimpleRTK5: Failed to initialize chip.\n");
        goto error_cfg;
    }
    
    if (!setupMediumDict()) {
        IOLog("SimpleRTK5: Failed to setup medium dictionary.\n");
        goto error_cfg;
    }
    commandGate = getCommandGate();
    
    if (!commandGate) {
        IOLog("SimpleRTK5: getCommandGate() failed.\n");
        goto error_gate;
    }
    commandGate->retain();
    
    /* 分配发送、接收和统计资源 */
    if (!setupTxResources()) {
        IOLog("SimpleRTK5: Error allocating Tx resources.\n");
        goto error_dma1;
    }

    if (!setupRxResources()) {
        IOLog("SimpleRTK5: Error allocating Rx resources.\n");
        goto error_dma2;
    }

    if (!setupStatResources()) {
        IOLog("SimpleRTK5: Error allocating Stat resources.\n");
        goto error_dma3;
    }

    if (!initEventSources(provider)) {
        IOLog("SimpleRTK5: initEventSources() failed.\n");
        goto error_src;
    }
    
    result = attachInterface(reinterpret_cast<IONetworkInterface**>(&netif));

    if (!result) {
        IOLog("SimpleRTK5: attachInterface() failed.\n");
        goto error_src;
    }
    pciDevice->close(this);
    result = true;
    
done:
    return result;

error_src:
    freeStatResources();

error_dma3:
    freeRxResources();

error_dma2:
    freeTxResources();
    
error_dma1:
    RELEASE(commandGate);
        
error_gate:
    RELEASE(mediumDict);

error_cfg:
    pciDevice->close(this);
    
error_open:
    pciDevice->release();
    pciDevice = NULL;
    goto done;
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

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

IOReturn SimpleRTK5::registerWithPolicyMaker(IOService *policyMaker)
{
    DebugLog("SimpleRTK5: registerWithPolicyMaker() ===>\n");
    
    powerState = kPowerStateOn;
    
    DebugLog("SimpleRTK5: registerWithPolicyMaker() <===\n");

    return policyMaker->registerPowerDriver(this, powerStateArray, kPowerStateCount);
}

IOReturn SimpleRTK5::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
    
    DebugLog("SimpleRTK5: setPowerState() ===>\n");
        
    if (powerStateOrdinal == powerState) {
        DebugLog("SimpleRTK5: Already in power state %lu.\n", powerStateOrdinal);
        goto done;
    }
    DebugLog("SimpleRTK5: switching to power state %lu.\n", powerStateOrdinal);
    
    if (powerStateOrdinal == kPowerStateOff)
        commandGate->runAction(setPowerStateSleepAction);
    else
        commandGate->runAction(setPowerStateWakeAction);

    powerState = powerStateOrdinal;
    
done:
    DebugLog("SimpleRTK5: setPowerState() <===\n");

    return result;
}

void SimpleRTK5::systemWillShutdown(IOOptionBits specifier)
{
    DebugLog("SimpleRTK5: systemWillShutdown() ===>\n");
    
    if ((kIOMessageSystemWillPowerOff | kIOMessageSystemWillRestart) & specifier) {
        disable(netif);
        
        /* 恢复原始MAC地址 */
        rtl8126_rar_set(&linuxData, (UInt8 *)&origMacAddr.bytes);
    }
    
    DebugLog("SimpleRTK5: systemWillShutdown() <===\n");

    super::systemWillShutdown(specifier);
}

IOReturn SimpleRTK5::enable(IONetworkInterface *netif)
{
    const IONetworkMedium *selectedMedium;
    IOReturn result = kIOReturnError;
    
    DebugLog("SimpleRTK5: enable() ===>\n");

    if (test_bit(__ENABLED, &stateFlags)) {
        DebugLog("SimpleRTK5: Interface already enabled.\n");
        result = kIOReturnSuccess;
        goto done;
    }
    if (!pciDevice || pciDevice->isOpen()) {
        IOLog("SimpleRTK5: Unable to open PCI device.\n");
        goto done;
    }
    pciDevice->open(this);
    
    selectedMedium = getSelectedMedium();
    
    if (!selectedMedium) {
        DebugLog("SimpleRTK5: No medium selected. Falling back to autonegotiation.\n");
        selectedMedium = mediumTable[MEDIUM_INDEX_AUTO];
    }
    selectMedium(selectedMedium);
    enableRTL8126();
    
    /* 启用MSI中断 */
    interruptSource->enable();

    txDescDoneCount = txDescDoneLast = 0;
    deadlockWarn = 0;
    needsUpdate = false;
    set_bit(__ENABLED, &stateFlags);
    clear_bit(__POLL_MODE, &stateFlags);

    result = kIOReturnSuccess;
    
    DebugLog("SimpleRTK5: enable() <===\n");

done:
    return result;
}

IOReturn SimpleRTK5::disable(IONetworkInterface *netif)
{
    UInt64 timeout;
    UInt64 delay;
    UInt64 now;
    UInt64 t;

    DebugLog("SimpleRTK5: disable() ===>\n");
    
    if (!test_bit(__ENABLED, &stateFlags))
        goto done;
    
    netif->stopOutputThread();
    netif->flushOutputQueue();
    
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

    /* 禁用中断 */
    interruptSource->disable();

    disableRTL8126();
    
    clearRxTxRings();
    
    if (pciDevice && pciDevice->isOpen())
        pciDevice->close(this);
        
    DebugLog("SimpleRTK5: disable() <===\n");
    
done:
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::outputStart(IONetworkInterface *interface, IOOptionBits options )
{
    IOPhysicalSegment txSegments[kMaxSegs];
    mbuf_t m;
    RtlTxDesc *desc, *firstDesc;
    IOReturn result = kIOReturnNoResources;
    UInt32 cmd;
    UInt32 opts2;
    UInt32 offloadFlags;
    UInt32 mss;
    UInt32 len;
    UInt32 tcpOff;
    UInt32 opts1;
    UInt32 vlanTag;
    UInt32 numSegs;
    UInt32 lastSeg;
    UInt32 index;
    UInt32 i;
    
    if (!(test_mask((__ENABLED_M | __LINK_UP_M), &stateFlags)))  {
        DebugLog("SimpleRTK5: Interface down. Dropping packets.\n");
        goto done;
    }
    while ((txNumFreeDesc > (kMaxSegs + 3)) && (interface->dequeueOutputPackets(1, &m, NULL, NULL, NULL) == kIOReturnSuccess)) {
        cmd = 0;
        opts2 = 0;

        /* 获取包长度 */
        len = (UInt32)mbuf_pkthdr_len(m);

        if (mbuf_get_tso_requested(m, &offloadFlags, &mss)) {
            DebugLog("SimpleRTK5: mbuf_get_tso_requested() failed. Dropping packet.\n");
            freePacket(m);
            continue;
        }
        /* 处理TSO卸载 */
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
            /* 处理Checksum卸载 */
            mbuf_get_csum_requested(m, &offloadFlags, &mss);
            
            if (offloadFlags & kChecksumTCP)
                opts2 = (TxIPCS_C | TxTCPCS_C);
            else if (offloadFlags & kChecksumTCPIPv6)
                opts2 = (TxTCPCS_C | TxIPV6F_C | (((kMacHdrLen + kIPv6HdrLen) & TCPHO_MAX) << TCPHO_SHIFT));
            else if (offloadFlags & kChecksumUDP)
                opts2 = (TxIPCS_C | TxUDPCS_C);
            else if (offloadFlags & kChecksumUDPIPv6)
                opts2 = (TxUDPCS_C | TxIPV6F_C | (((kMacHdrLen + kIPv6HdrLen) & TCPHO_MAX) << TCPHO_SHIFT));
            else if (offloadFlags & kChecksumIP)
                opts2 = TxIPCS_C;
        }
        /* 获取物理段 */
        numSegs = txMbufCursor->getPhysicalSegmentsWithCoalesce(m, &txSegments[0], kMaxSegs);

        if (!numSegs) {
            DebugLog("SimpleRTK5: getPhysicalSegmentsWithCoalesce() failed. Dropping packet.\n");
            freePacket(m);
            continue;
        }
        OSAddAtomic(-numSegs, &txNumFreeDesc);
        index = txNextDescIndex;
        txNextDescIndex = (txNextDescIndex + numSegs) & kTxDescMask;
        txTailPtr0 += numSegs;
        firstDesc = &txDescArray[index];
        lastSeg = numSegs - 1;
        
        /* 填充VLAN标签 */
        opts2 |= (getVlanTagDemand(m, &vlanTag)) ? (OSSwapInt16(vlanTag) | TxVlanTag) : 0;
        
        /* 填充描述符 */
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
    /* 更新尾部指针 */
    WriteReg16(SW_TAIL_PTR0_8125, txTailPtr0 & 0xffff);

    result = (txNumFreeDesc > (kMaxSegs + 3)) ? kIOReturnSuccess : kIOReturnNoResources;
    
done:
    return result;
}

void SimpleRTK5::getPacketBufferConstraints(IOPacketBufferConstraints *constraints) const
{
    DebugLog("SimpleRTK5: getPacketBufferConstraints() ===>\n");

    constraints->alignStart = kIOPacketBufferAlign1;
    constraints->alignLength = kIOPacketBufferAlign1;
    
    DebugLog("SimpleRTK5: getPacketBufferConstraints() <===\n");
}

IOOutputQueue* SimpleRTK5::createOutputQueue()
{
    DebugLog("SimpleRTK5: createOutputQueue() ===>\n");
    DebugLog("SimpleRTK5: createOutputQueue() <===\n");
    return IOBasicOutputQueue::withTarget(this);
}

const OSString* SimpleRTK5::newVendorString() const
{
    DebugLog("SimpleRTK5: newVendorString() ===>\n");
    DebugLog("SimpleRTK5: newVendorString() <===\n");
    return OSString::withCString("Realtek");
}

const OSString* SimpleRTK5::newModelString() const
{
    DebugLog("SimpleRTK5: newModelString() ===>\n");
    DebugLog("SimpleRTK5: newModelString() <===\n");
    /* 更新型号字符串以反映2.5G/5G支持 */
    return OSString::withCString(rtl_chip_info[linuxData.chipset].name);
}

bool SimpleRTK5::configureInterface(IONetworkInterface *interface)
{
    char modelName[kNameLenght];
    IONetworkData *data;
    IOReturn error;
    bool result;

    DebugLog("SimpleRTK5: configureInterface() ===>\n");

    result = super::configureInterface(interface);
    
    if (!result)
        goto done;
    
    /* 获取网络统计结构 */
    data = interface->getParameter(kIONetworkStatsKey);
    
    if (data) {
        netStats = (IONetworkStats *)data->getBuffer();
        
        if (!netStats) {
            IOLog("SimpleRTK5: Error getting IONetworkStats\n.");
            result = false;
            goto done;
        }
    }
    /* 获取以太网统计结构 */
    data = interface->getParameter(kIOEthernetStatsKey);
    
    if (data) {
        etherStats = (IOEthernetStats *)data->getBuffer();
        
        if (!etherStats) {
            IOLog("SimpleRTK5: Error getting IOEthernetStats\n.");
            result = false;
            goto done;
        }
    }
    error = interface->configureOutputPullModel((kNumTxDesc/2), 0, 0, IONetworkInterface::kOutputPacketSchedulingModelNormal);
    
    if (error != kIOReturnSuccess) {
        IOLog("SimpleRTK5: configureOutputPullModel() failed\n.");
        result = false;
        goto done;
    }
    error = interface->configureInputPacketPolling(kNumRxDesc, 0);
    
    if (error != kIOReturnSuccess) {
        IOLog("SimpleRTK5: configureInputPacketPolling() failed\n.");
        result = false;
        goto done;
    }
    /* 动态设置型号名称 */
    snprintf(modelName, kNameLenght, "Realtek %s PCIe 2.5/5 Gbit Ethernet", rtl_chip_info[linuxData.chipset].name);
    setProperty("model", modelName);
    
    DebugLog("SimpleRTK5: configureInterface() <===\n");

done:
    return result;
}

bool SimpleRTK5::createWorkLoop()
{
    DebugLog("SimpleRTK5: createWorkLoop() ===>\n");
    workLoop = IOWorkLoop::workLoop();
    DebugLog("SimpleRTK5: createWorkLoop() <===\n");
    return workLoop ? true : false;
}

IOWorkLoop* SimpleRTK5::getWorkLoop() const
{
    DebugLog("SimpleRTK5: getWorkLoop() ===>\n");
    DebugLog("SimpleRTK5: getWorkLoop() <===\n");
    return workLoop;
}

IOReturn SimpleRTK5::getHardwareAddress(IOEthernetAddress *addr)
{
    IOReturn result = kIOReturnError;
    
    DebugLog("SimpleRTK5: getHardwareAddress() ===>\n");
    
    if (addr) {
        bcopy(&currMacAddr.bytes, addr->bytes, kIOEthernetAddressSize);
        result = kIOReturnSuccess;
    }
    
    DebugLog("SimpleRTK5: getHardwareAddress() <===\n");

    return result;
}

IOReturn SimpleRTK5::setPromiscuousMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;

    DebugLog("SimpleRTK5: setPromiscuousMode() ===>\n");
    
    if (active) {
        DebugLog("SimpleRTK5: Promiscuous mode enabled.\n");
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys);
        mcFilter[1] = mcFilter[0] = 0xffffffff;
    } else {
        DebugLog("SimpleRTK5: Promiscuous mode disabled.\n");
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
    }
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR0 + 4, mcFilter[1]);

    if (active)
        set_bit(__PROMISC, &stateFlags);
    else
        clear_bit(__PROMISC, &stateFlags);

    DebugLog("SimpleRTK5: setPromiscuousMode() <===\n");

    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMulticastMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;

    DebugLog("SimpleRTK5: setMulticastMode() ===>\n");
    
    if (active) {
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
    } else{
        rxMode = (AcceptBroadcast | AcceptMyPhys);
        mcFilter[1] = mcFilter[0] = 0;
    }
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR0 + 4, mcFilter[1]);
    
    if (active)
        set_bit(__M_CAST, &stateFlags);
    else
        clear_bit(__M_CAST, &stateFlags);

    DebugLog("SimpleRTK5: setMulticastMode() <===\n");
    
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt64 filter = 0;
    UInt32 i, bitNumber;
    
    DebugLog("SimpleRTK5: setMulticastList() ===>\n");
    
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

    DebugLog("SimpleRTK5: setMulticastList() <===\n");

    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput)
{
    IOReturn result = kIOReturnUnsupported;

    DebugLog("SimpleRTK5: getChecksumSupport() ===>\n");

    if ((checksumFamily == kChecksumFamilyTCPIP) && checksumMask) {
        if (isOutput) {
            *checksumMask = (enableCSO6) ? (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6) : (kChecksumTCP | kChecksumUDP | kChecksumIP);
        } else {
            *checksumMask = (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6);
        }
        result = kIOReturnSuccess;
    }
    DebugLog("SimpleRTK5: getChecksumSupport() <===\n");

    return result;
}

UInt32 SimpleRTK5::getFeatures() const
{
    UInt32 features = (kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan);
    
    DebugLog("SimpleRTK5: getFeatures() ===>\n");
    
    if (enableTSO4)
        features |= kIONetworkFeatureTSOIPv4;
    
    if (enableTSO6)
        features |= kIONetworkFeatureTSOIPv6;
    
    DebugLog("SimpleRTK5: getFeatures() <===\n");
    
    return features;
}

IOReturn SimpleRTK5::setWakeOnMagicPacket(bool active)
{
    IOReturn result = kIOReturnUnsupported;

    DebugLog("SimpleRTK5: setWakeOnMagicPacket() ===>\n");

    if (wolCapable) {
        linuxData.wol_enabled = active ? WOL_ENABLED : WOL_DISABLED;
        wolActive = active;
        
        DebugLog("SimpleRTK5: WakeOnMagicPacket %s.\n", active ? "enabled" : "disabled");

        result = kIOReturnSuccess;
    }
    
    DebugLog("SimpleRTK5: setWakeOnMagicPacket() <===\n");

    return result;
}

IOReturn SimpleRTK5::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn result = kIOReturnSuccess;

    DebugLog("SimpleRTK5: getPacketFilters() ===>\n");

    if ((group == gIOEthernetWakeOnLANFilterGroup) && wolCapable) {
        *filters = kIOEthernetWakeOnMagicPacket;
        DebugLog("SimpleRTK5: kIOEthernetWakeOnMagicPacket added to filters.\n");
    } else {
        result = super::getPacketFilters(group, filters);
    }
    
    DebugLog("SimpleRTK5: getPacketFilters() <===\n");

    return result;
}

IOReturn SimpleRTK5::setHardwareAddress(const IOEthernetAddress *addr)
{
    IOReturn result = kIOReturnError;
    
    DebugLog("SimpleRTK5: setHardwareAddress() ===>\n");
    
    if (addr) {
        bcopy(addr->bytes, &currMacAddr.bytes, kIOEthernetAddressSize);
        rtl8126_rar_set(&linuxData, (UInt8 *)&currMacAddr.bytes);
        result = kIOReturnSuccess;
    }
    
    DebugLog("SimpleRTK5: setHardwareAddress() <===\n");
    
    return result;
}

IOReturn SimpleRTK5::selectMedium(const IONetworkMedium *medium)
{
    IOReturn result = kIOReturnSuccess;
    
    DebugLog("SimpleRTK5: selectMedium() ===>\n");
    
    if (medium) {
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

            /* 增加5G速率支持 */
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
    }
    
    DebugLog("SimpleRTK5: selectMedium() <===\n");
    
done:
    return result;
}

IOReturn SimpleRTK5::getMaxPacketSize(UInt32 * maxSize) const
{
    DebugLog("SimpleRTK5: getMaxPacketSize() ===>\n");
        
    if (version_major >= 22) {
        *maxSize = rxBufferSize - 2;
    } else {
        *maxSize = kMaxPacketSize;
    }
    DebugLog("SimpleRTK5: maxSize: %u, version_major: %u\n", *maxSize, version_major);

    DebugLog("SimpleRTK5: getMaxPacketSize() <===\n");
    
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setMaxPacketSize(UInt32 maxSize)
{
    ifnet_t ifnet = netif->getIfnet();
    ifnet_offload_t offload;
    UInt32 mask = 0;
    IOReturn result = kIOReturnError;

    DebugLog("SimpleRTK5: setMaxPacketSize() ===>\n");
    
    if (maxSize <= (rxBufferSize - 2)) {
        mtu = maxSize - (ETH_HLEN + ETH_FCS_LEN);
        DebugLog("SimpleRTK5: maxSize: %u, mtu: %u\n", maxSize, mtu);
        
        if (enableTSO4)
            mask |= IFNET_TSO_IPV4;
        
        if (enableTSO6)
            mask |= IFNET_TSO_IPV6;

        if (enableCSO6)
            mask |= (IFNET_CSUM_TCPIPV6 | IFNET_CSUM_UDPIPV6);

        offload = ifnet_offload(ifnet);
        
        if (mtu > MSS_MAX) {
            offload &= ~mask;
            DebugLog("SimpleRTK5: Disable hardware offload features: %x!\n", mask);
        } else {
            offload |= mask;
            DebugLog("SimpleRTK5: Enable hardware offload features: %x!\n", mask);
        }
        if (ifnet_set_offload(ifnet, offload))
            IOLog("SimpleRTK5: Error setting hardware offload: %x!\n", offload);
        
        /* 强制重新初始化 */
        setLinkDown();
        timerSource->cancelTimeout();
        restartRTL8126();
        
        result = kIOReturnSuccess;
    }
    
    DebugLog("SimpleRTK5: setMaxPacketSize() <===\n");
    
    return result;
}

void SimpleRTK5::pciErrorInterrupt()
{
    UInt16 cmdReg = pciDevice->configRead16(kIOPCIConfigCommand);
    UInt16 statusReg = pciDevice->configRead16(kIOPCIConfigStatus);
    
    DebugLog("SimpleRTK5: PCI error: cmdReg=0x%x, statusReg=0x%x\n", cmdReg, statusReg);

    cmdReg |= (kIOPCICommandSERR | kIOPCICommandParityError);
    statusReg &= (kIOPCIStatusParityErrActive | kIOPCIStatusSERRActive | kIOPCIStatusMasterAbortActive | kIOPCIStatusTargetAbortActive | kIOPCIStatusTargetAbortCapable);
    pciDevice->configWrite16(kIOPCIConfigCommand, cmdReg);
    pciDevice->configWrite16(kIOPCIConfigStatus, statusReg);
    
    /* 重置网卡以恢复操作 */
    restartRTL8126();
}

void SimpleRTK5::txInterrupt()
{
    mbuf_t m;
    UInt32 nextClosePtr = ReadReg16(HW_CLO_PTR0_8125);
    UInt32 oldDirtyIndex = txDirtyDescIndex;
    UInt32 numDone;

    numDone = ((nextClosePtr - txClosePtr0) & 0xffff);
    
    txClosePtr0 = nextClosePtr;

    while (numDone-- > 0) {
        m = txMbufArray[txDirtyDescIndex];
        txMbufArray[txDirtyDescIndex] = NULL;

        if (m)
            freePacket(m, kDelayFree);

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

UInt32 SimpleRTK5::rxInterrupt(IONetworkInterface *interface, uint32_t maxCount, IOMbufQueue *pollQueue, void *context)
{
    IOPhysicalSegment rxSegment;
    RtlRxDesc *desc = &rxDescArray[rxNextDescIndex];
    mbuf_t bufPkt, newPkt;
    UInt64 addr;
    UInt32 opts1, opts2;
    UInt32 descStatus1, descStatus2;
    UInt32 pktSize;
    UInt32 goodPkts = 0;
    bool replaced;
    
    while (!((descStatus1 = OSSwapLittleToHostInt32(desc->opts1)) & DescOwn) && (goodPkts < maxCount)) {
        opts1 = (rxNextDescIndex == kRxLastDesc) ? (RingEnd | DescOwn) : DescOwn;
        opts2 = 0;
        addr = 0;
        
        /* 检查分片包错误 */
        if (unlikely((descStatus1 & (FirstFrag|LastFrag)) != (FirstFrag|LastFrag))) {
            DebugLog("SimpleRTK5: Fragmented packet.\n");
            etherStats->dot3StatsEntry.frameTooLongs++;
            opts1 |= rxBufferSize;
            goto nextDesc;
        }
        
        /* 检查接收错误 */
        if (unlikely(descStatus1 & RxRES)) {
            DebugLog("SimpleRTK5: Rx error.\n");
            
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
        
        newPkt = replaceOrCopyPacket(&bufPkt, pktSize, &replaced);
        
        if (unlikely(!newPkt)) {
            /* 分配失败，尝试使用备用包 */
            if (spareNum > 1) {
                DebugLog("SimpleRTK5: Use spare packet to replace buffer (%d available).\n", spareNum);
                OSDecrementAtomic(&spareNum);

                newPkt = bufPkt;
                replaced = true;

                bufPkt = sparePktHead;
                sparePktHead = mbuf_next(sparePktHead);
                mbuf_setnext(bufPkt, NULL);
                goto handle_pkt;
            }
            /* 无备用包，丢弃当前包 */
            DebugLog("SimpleRTK5: replaceOrCopyPacket() failed.\n");
            etherStats->dot3RxExtraEntry.resourceErrors++;
            opts1 |= rxBufferSize;
            goto nextDesc;
        }
handle_pkt:
        /* 如果包被替换，更新描述符缓冲地址 */
        if (replaced) {
            if (rxMbufCursor->getPhysicalSegments(bufPkt, &rxSegment, 1) != 1) {
                DebugLog("SimpleRTK5: getPhysicalSegments() failed.\n");
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
        /* 设置缓冲区长度 */
        mbuf_setlen(newPkt, pktSize);

        getChecksumResult(newPkt, descStatus1, descStatus2);

        /* 获取VLAN标签 */
        if (descStatus2 & RxVlanTag)
            setVlanTag(newPkt, OSSwapInt16(descStatus2 & 0xffff));

        mbuf_pkthdr_setlen(newPkt, pktSize);
        interface->enqueueInputPacket(newPkt, pollQueue);
        goodPkts++;
        
    nextDesc:
        if (addr)
            desc->addr = OSSwapHostToLittleInt64(addr);
        
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
    
    DebugLog("SimpleRTK5: Link change interrupt: Check link status.\n");

    currLinkState = ReadReg16(PHYstatus);
    
    if (currLinkState & LinkStatus) {
        eeeMode = getEEEMode();
        
        /* 获取链路速率、双工和流控模式 */
        if (currLinkState & (TxFlowCtrl | RxFlowCtrl)) {
            flowCtl = kFlowControlOn;
        } else {
            flowCtl = kFlowControlOff;
        }

        /* 增加5G速率判断 */
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
            
            if (currLinkState & FullDup) {
                duplex = DUPLEX_FULL;
            } else {
                duplex = DUPLEX_HALF;
            }
        } else {
            speed = SPEED_10;
            
            if (currLinkState & FullDup) {
                duplex = DUPLEX_FULL;
            } else {
                duplex = DUPLEX_HALF;
            }
        }
        setupRTL8126();
        
        switch (tp->mcfg) {
        case CFG_METHOD_1:
        case CFG_METHOD_2:
        case CFG_METHOD_3:
                if (RTL_R8(tp, PHYstatus) & _10bps)
                        rtl8126_enable_eee_plus(tp);
                break;
        default:
                break;
        }

        setLinkUp();
        timerSource->setTimeoutMS(kTimeoutMS);
        
        rtl8126_mdio_write(tp, 0x1F, 0x0000);
        linuxData.phy_reg_anlpar = rtl8126_mdio_read(tp, MII_LPA);
    } else {
        tp->phy_reg_anlpar = 0;
        
        rtl8126_disable_eee_plus(tp);

        /* Stop watchdog and statistics updates. */
        timerSource->cancelTimeout();
        setLinkDown();
    }
}

void SimpleRTK5::interruptHandler(OSObject *client, IOInterruptEventSource *src, int count)
{
    UInt32 packets;
    UInt32 status;
    
    status = ReadReg32(ISR0_8125);
    
    /* 热插拔/重大错误/无工作/共享IRQ */
    if ((status == 0xFFFFFFFF) || !status)
        goto done;
    
    WriteReg32(IMR0_8125, 0x0000);
    WriteReg32(ISR0_8125, (status & ~RxFIFOOver));

    if (status & SYSErr) {
        pciErrorInterrupt();
        goto done;
    }
    if (!test_bit(__POLL_MODE, &stateFlags) &&
        !test_and_set_bit(__POLLING, &stateFlags)) {
        /* 接收中断 */
        if (status & (RxOK | RxDescUnavail)) {
            packets = rxInterrupt(netif, kNumRxDesc, NULL, NULL);
            
            if (packets)
                netif->flushInputQueue();
            
            etherStats->dot3RxExtraEntry.interrupts++;
            
            if (spareNum < kRxNumSpareMbufs)
                refillSpareBuffers();
        }
        /* 发送中断 */
        if (status & (TxOK | RxOK | PCSTimeout)) {
            txInterrupt();
            
            if (status & TxOK)
                etherStats->dot3TxExtraEntry.interrupts++;
        }
        if (status & (TxOK | RxOK)) {
            WriteReg32(TIMER_INT0_8125, 0x5000);
            WriteReg32(TCTR0_8125, 0x5000);
            intrMask = intrMaskTimer;
        } else if (status & PCSTimeout) {
            WriteReg32(TIMER_INT0_8125, 0x0000);
            intrMask = intrMaskRxTx;
        }
#ifdef DEBUG
        if (status & PCSTimeout)
            tmrInterrupts++;
#endif
        
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
    bool deadlock = false;
    
    if ((txDescDoneCount == txDescDoneLast) && (txNumFreeDesc < kNumTxDesc)) {
        if (++deadlockWarn == kTxCheckTreshhold) {
            DebugLog("SimpleRTK5: Warning: Tx timeout, ISR0=0x%x, IMR0=0x%x, polling=%u.\n", ReadReg32(ISR0_8125),
                     ReadReg32(IMR0_8125), test_bit(__POLL_MODE, &stateFlags));
            etherStats->dot3TxExtraEntry.timeouts++;
            txInterrupt();
        } else if (deadlockWarn >= kTxDeadlockTreshhold) {
#ifdef DEBUG
            UInt32 i, index;
            
            for (i = 0; i < 10; i++) {
                index = ((txDirtyDescIndex - 1 + i) & kTxDescMask);
                IOLog("SimpleRTK5: desc[%u]: opts1=0x%x, opts2=0x%x, addr=0x%llx.\n", index,
                      txDescArray[index].opts1, txDescArray[index].opts2, txDescArray[index].addr);
            }
#endif
            IOLog("SimpleRTK5: Tx stalled? Resetting chipset. ISR0=0x%x, IMR0=0x%x.\n", ReadReg32(ISR0_8125),
                  ReadReg32(IMR0_8125));
            etherStats->dot3TxExtraEntry.resets++;
            restartRTL8126();
            deadlock = true;
        }
    } else {
        deadlockWarn = 0;
    }
    return deadlock;
}

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
    DebugLog("SimpleRTK5: Input polling %s.\n", enabled ? "enabled" : "disabled");
    
    return kIOReturnSuccess;
}

void SimpleRTK5::pollInputPackets(IONetworkInterface *interface, uint32_t maxCount, IOMbufQueue *pollQueue, void *context )
{
    if (test_bit(__POLL_MODE, &stateFlags) &&
        !test_and_set_bit(__POLLING, &stateFlags)) {

        rxInterrupt(interface, maxCount, pollQueue, context);
        
        txInterrupt();
        
        clear_bit(__POLLING, &stateFlags);
        
        if (spareNum < kRxNumSpareMbufs)
            commandGate->runAction(refillAction);
    }
}

inline void SimpleRTK5::getChecksumResult(mbuf_t m, UInt32 status1, UInt32 status2)
{
    mbuf_csum_performed_flags_t performed = 0;
    UInt32 value = 0;

    if ((status2 & RxV4F) && !(status1 & RxIPF))
        performed |= (MBUF_CSUM_DID_IP | MBUF_CSUM_IP_GOOD);

    if (((status1 & RxTCPT) && !(status1 & RxTCPF)) ||
        ((status1 & RxUDPT) && !(status1 & RxUDPF))) {
        performed |= (MBUF_CSUM_DID_DATA | MBUF_CSUM_PSEUDO_HDR);
        value = 0xffff; // fake a valid checksum value
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

    /* 获取链路速率、双工和流控模式 */
    if (flowCtl == kFlowControlOn) {
        flowName = onFlowName;
    } else {
        flowName = offFlowName;
    }
    
    /* 增加5G处理逻辑 */
    if (speed == SPEED_5000) {
        mediumSpeed = kSpeed5000MBit;
        speedName = speed5GName;
        duplexName = duplexFullName;
       
        if (flowCtl == kFlowControlOn) {
            mediumIndex = MEDIUM_INDEX_5000FDFC;
        } else {
            mediumIndex = MEDIUM_INDEX_5000FD;
        }
    } else if (speed == SPEED_2500) {
        mediumSpeed = kSpeed2500MBit;
        speedName = speed25GName;
        duplexName = duplexFullName;
       
        if (flowCtl == kFlowControlOn) {
            mediumIndex = MEDIUM_INDEX_2500FDFC;
        } else {
            mediumIndex = MEDIUM_INDEX_2500FD;
        }
    } else if (speed == SPEED_1000) {
        mediumSpeed = kSpeed1000MBit;
        speedName = speed1GName;
        duplexName = duplexFullName;
       
        if (flowCtl == kFlowControlOn) {
            if (eeeMode & MDIO_EEE_1000T) {
                mediumIndex = MEDIUM_INDEX_1000FDFCEEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MEDIUM_INDEX_1000FDFC;
            }
        } else {
            if (eeeMode & MDIO_EEE_1000T) {
                mediumIndex = MEDIUM_INDEX_1000FDEEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MEDIUM_INDEX_1000FD;
            }
        }
    } else if (speed == SPEED_100) {
        mediumSpeed = kSpeed100MBit;
        speedName = speed100MName;
        
        if (duplex == DUPLEX_FULL) {
            duplexName = duplexFullName;
            
            if (flowCtl == kFlowControlOn) {
                if (eeeMode & MDIO_EEE_100TX) {
                    mediumIndex =  MEDIUM_INDEX_100FDFCEEE;
                    eeeName = eeeNames[kEEETypeYes];
                } else {
                    mediumIndex = MEDIUM_INDEX_100FDFC;
                }
            } else {
                if (eeeMode & MDIO_EEE_100TX) {
                    mediumIndex =  MEDIUM_INDEX_100FDEEE;
                    eeeName = eeeNames[kEEETypeYes];
                } else {
                    mediumIndex = MEDIUM_INDEX_100FD;
                }
            }
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
    /* 启用接收和发送 */
    WriteReg8(ChipCmd, CmdTxEnb | CmdRxEnb);

    set_bit(__LINK_UP, &stateFlags);
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, mediumTable[mediumIndex], mediumSpeed, NULL);

    /* 启动输出线程，统计更新和看门狗 */
    bzero(&pParams, sizeof(IONetworkPacketPollingParameters));
    
    if (speed == SPEED_10) {
        pParams.lowThresholdPackets = 2;
        pParams.highThresholdPackets = 8;
        pParams.lowThresholdBytes = 0x400;
        pParams.highThresholdBytes = 0x1800;
        pParams.pollIntervalTime = 1000000;  /* 1ms */
    } else {
        pParams.lowThresholdPackets = 10;
        pParams.highThresholdPackets = 40;
        pParams.lowThresholdBytes = 0x1000;
        pParams.highThresholdBytes = 0x10000;
        
        /* 5G和2.5G共用轮询参数 */
        if (speed == SPEED_5000)
            pParams.pollIntervalTime = pollInterval2500;
        else if (speed == SPEED_2500)
            pParams.pollIntervalTime = pollInterval2500;
        else if (speed == SPEED_1000)
            pParams.pollIntervalTime = 170000;   /* 170µs */
        else
            pParams.pollIntervalTime = 1000000;  /* 1ms */
    }
    netif->setPacketPollingParameters(&pParams, 0);
    DebugLog("SimpleRTK5: pollIntervalTime: %lluµs\n", (pParams.pollIntervalTime / 1000));

    netif->startOutputThread();

    IOLog("SimpleRTK5: Link up on en%u, %s, %s, %s%s\n", netif->getUnitNumber(), speedName, duplexName, flowName, eeeName);
}

void SimpleRTK5::setLinkDown()
{
    deadlockWarn = 0;
    needsUpdate = false;

    /* 停止输出线程并刷新队列 */
    netif->stopOutputThread();
    netif->flushOutputQueue();

    /* 更新链路状态 */
    clear_mask((__LINK_UP_M | __POLL_MODE_M), &stateFlags);
    setLinkStatus(kIONetworkLinkValid);

    rtl8126_nic_reset(&linuxData);

    /* 清理描述符环 */
    clearRxTxRings();
    
    setPhyMedium();
    
    IOLog("SimpleRTK5: Link down on en%u\n", netif->getUnitNumber());
}

void SimpleRTK5::updateStatitics()
{
    UInt32 sgColl, mlColl;
    UInt32 cmd;

    /* 检查统计数据转储是否完成 */
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
#ifdef DEBUG
    UInt32 rxIntr = etherStats->dot3RxExtraEntry.interrupts - lastRxIntrupts;
    UInt32 txIntr = etherStats->dot3TxExtraEntry.interrupts - lastTxIntrupts;
    UInt32 tmrIntr = tmrInterrupts - lastTmrIntrupts;

    lastRxIntrupts = etherStats->dot3RxExtraEntry.interrupts;
    lastTxIntrupts = etherStats->dot3TxExtraEntry.interrupts;
    lastTmrIntrupts = tmrInterrupts;
#endif
    
    updateStatitics();

    if (!test_bit(__LINK_UP, &stateFlags))
        goto done;

    /* 检查发送死锁 */
    if (txHangCheck())
        goto done;
    
    timerSource->setTimeoutMS(kTimeoutMS);
        
done:
    txDescDoneLast = txDescDoneCount;
}

static inline void prepareTSO4(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss)
{
    UInt8 *p = (UInt8 *)mbuf_data(m) + kMacHdrLen;
    struct ip4_hdr_be *ip = (struct ip4_hdr_be *)p;
    struct tcp_hdr_be *tcp;
    UInt32 csum32 = 6;
    UInt32 i, il, tl;
    
    for (i = 0; i < 4; i++) {
        csum32 += ntohs(ip->addr[i]);
        csum32 += (csum32 >> 16);
        csum32 &= 0xffff;
    }
    il = ((ip->hdr_len & 0x0f) << 2);
    
    tcp = (struct tcp_hdr_be *)(p + il);
    tl = ((tcp->dat_off & 0xf0) >> 2);

    /* 填充TSOv4的伪首部校验和 */
    tcp->csum = htons((UInt16)csum32);

    *tcpOffset = kMacHdrLen + il;
    
    if (*mss > MSS_MAX)
        *mss = MSS_MAX;
}

static inline void prepareTSO6(mbuf_t m, UInt32 *tcpOffset, UInt32 *mss)
{
    UInt8 *p = (UInt8 *)mbuf_data(m) + kMacHdrLen;
    struct ip6_hdr_be *ip6 = (struct ip6_hdr_be *)p;
    struct tcp_hdr_be *tcp;
    UInt32 csum32 = 6;
    UInt32 i, tl;

    ip6->pay_len = 0;

    for (i = 0; i < 16; i++) {
        csum32 += ntohs(ip6->addr[i]);
        csum32 += (csum32 >> 16);
        csum32 &= 0xffff;
    }
    
    tcp = (struct tcp_hdr_be *)(p + kIPv6HdrLen);
    tl = ((tcp->dat_off & 0xf0) >> 2);

    /* 填充TSOv6的伪首部校验和 */
    tcp->csum = htons((UInt16)csum32);

    *tcpOffset = kMacHdrLen + kIPv6HdrLen;
    
    if (*mss > MSS_MAX)
        *mss = MSS_MAX;
}

static unsigned const ethernet_polynomial = 0x04c11db7U;

static inline u32 ether_crc(int length, unsigned char *data)
{
    int crc = -1;
    
    while(--length >= 0) {
        unsigned char current_octet = *data++;
        int bit;
        for (bit = 0; bit < 8; bit++, current_octet >>= 1) {
            crc = (crc << 1) ^
            ((crc < 0) ^ (current_octet & 1) ? ethernet_polynomial : 0);
        }
    }
    return crc;
}

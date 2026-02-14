//
//  SimpleRTK5Hardware.cpp
//  SimpleRTK5
//
//  Created by laobamac on 2025/10/6.
//

/* RTL8125Hardware.hpp -- RTL812x hardware initialzation methods.
 *
 * Copyright (c) 2025 Laura Müller <laura-mueller@uni-duesseldorf.de>
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
 * Driver for Realtek RTL812x PCIe 2.5/5/10Gbit Ethernet controllers.
 *
 * This driver is based on version 9.016.01 of Realtek's r8125 driver.
 */

#include "SimpleRTK5Ethernet.hpp"
#include "linux/mdio.h"
#include "r8125_dash.h"
#include "rtl_eeprom.h"

#pragma mark--- static data ---

static const char *speed5GName = "5 Gigabit";
static const char *speed25GName = "2.5 Gigabit";
static const char *speed1GName = "1 Gigabit";
static const char *speed100MName = "100 Megabit";
static const char *speed10MName = "10 Megabit";
static const char *duplexFullName = "full-duplex";
static const char *duplexHalfName = "half-duplex";
static const char *offFlowName = "no flow-control";
static const char *onFlowName = "flow-control";

static const char *eeeNames[kEEETypeCount] = {"",
                                              ", energy-efficient-ethernet"};

#pragma mark--- PCIe configuration methods ---

bool SimpleRTK5::initPCIConfigSpace(IOPCIDevice *provider) {
    IOByteCount pmCapOffset;
    UInt32 pcieLinkCap;
    UInt16 cmdReg;
    UInt16 pmCap;
    bool result = false;

    /* Get vendor and device info. */
    pciDeviceData.vendor = provider->configRead16(kIOPCIConfigVendorID);
    pciDeviceData.device = provider->configRead16(kIOPCIConfigDeviceID);
    pciDeviceData.subsystem_vendor =
        provider->configRead16(kIOPCIConfigSubSystemVendorID);
    pciDeviceData.subsystem_device =
        provider->configRead16(kIOPCIConfigSubSystemID);

    /* Setup power management. */
    if (provider->extendedFindPCICapability(kIOPCIPowerManagementCapability,
                                            &pmCapOffset)) {
        pmCap =
            provider->extendedConfigRead16(pmCapOffset + kIOPCIPMCapability);
        DebugLog("SimpleRTK5: PCI power management capabilities: 0x%x.\n",
                 pmCap);

        if (pmCap & kPCIPMCPMESupportFromD3Cold) {
            wolCapable = true;
            DebugLog("SimpleRTK5: PME# from D3 (cold) supported.\n");
        }
        pciPMCtrlOffset = pmCapOffset + kIOPCIPMControl;
    } else {
        IOLog("SimpleRTK5: PCI power management unsupported.\n");
    }
    provider->enablePCIPowerManagement(kPCIPMCSPowerStateD0);

    /* Get PCIe link information. */
    if (provider->extendedFindPCICapability(kIOPCIPCIExpressCapability,
                                            &pcieCapOffset)) {
        pcieLinkCap = provider->extendedConfigRead32(pcieCapOffset +
                                                     kIOPCIELinkCapability);
        DebugLog("SimpleRTK5: PCIe link capability: 0x%08x.\n", pcieLinkCap);
    }
    /* Enable the device. */
    cmdReg = provider->configRead16(kIOPCIConfigCommand);
    cmdReg &= ~kIOPCICommandIOSpace;
    cmdReg |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace |
               kIOPCICommandMemWrInvalidate);
    provider->configWrite16(kIOPCIConfigCommand, cmdReg);

    baseMap = provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2,
                                                    kIOMapInhibitCache);

    if (!baseMap) {
        IOLog("SimpleRTK5: region #2 not an MMIO resource, aborting.\n");
        goto done;
    }
    linuxData.mmio_addr =
        reinterpret_cast<volatile void *>(baseMap->getVirtualAddress());

    linuxData.org_pci_offset_80 = provider->extendedConfigRead8(0x80);
    linuxData.org_pci_offset_81 = provider->extendedConfigRead8(0x81);

    result = true;

done:
    return result;
}

void SimpleRTK5::setupASPM(IOPCIDevice *provider, bool allowL1) {
    IOOptionBits aspmState = 0;
    UInt32 pcieLinkCap = 0;

    if (pcieCapOffset) {
        pcieLinkCap = provider->extendedConfigRead32(pcieCapOffset +
                                                     kIOPCIELinkCapability);
        DebugLog("SimpleRTK5: PCIe link capability: 0x%08x.\n", pcieLinkCap);

        if (enableASPM && (pcieLinkCap & kIOPCIELinkCapASPMCompl)) {
            if (pcieLinkCap & kIOPCIELinkCapL0sSup)
                aspmState |= kIOPCILinkControlASPMBitsL0s;

            if ((pcieLinkCap & kIOPCIELinkCapL1Sup) && allowL1)
                aspmState |= kIOPCILinkControlASPMBitsL1;

            IOLog("SimpleRTK5: Enable PCIe ASPM: 0x%08x.\n", aspmState);
        } else {
            IOLog("SimpleRTK5: Disable PCIe ASPM.\n");
        }
        provider->setASPMState(this, aspmState);
    }
}

IOReturn SimpleRTK5::setPowerStateWakeAction(OSObject *owner, void *arg1,
                                             void *arg2, void *arg3,
                                             void *arg4) {
    SimpleRTK5 *ethCtlr = OSDynamicCast(SimpleRTK5, owner);
    IOPCIDevice *dev;
    UInt16 val16;
    UInt8 offset;

    if (ethCtlr && ethCtlr->pciPMCtrlOffset) {
        dev = ethCtlr->pciDevice;
        offset = ethCtlr->pciPMCtrlOffset;

        val16 = dev->extendedConfigRead16(offset);

        val16 &=
            ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);
        val16 |= kPCIPMCSPowerStateD0;

        dev->extendedConfigWrite16(offset, val16);
    }
    return kIOReturnSuccess;
}

IOReturn SimpleRTK5::setPowerStateSleepAction(OSObject *owner, void *arg1,
                                              void *arg2, void *arg3,
                                              void *arg4) {
    SimpleRTK5 *ethCtlr = OSDynamicCast(SimpleRTK5, owner);
    IOPCIDevice *dev;
    UInt16 val16;
    UInt8 offset;

    if (ethCtlr && ethCtlr->pciPMCtrlOffset) {
        dev = ethCtlr->pciDevice;
        offset = ethCtlr->pciPMCtrlOffset;

        val16 = dev->extendedConfigRead16(offset);

        val16 &=
            ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);

        if (ethCtlr->linuxData.wol_enabled)
            val16 |=
                (kPCIPMCSPMEStatus | kPCIPMCSPMEEnable | kPCIPMCSPowerStateD3);
        else
            val16 |= kPCIPMCSPowerStateD3;

        dev->extendedConfigWrite16(offset, val16);
    }
    return kIOReturnSuccess;
}

bool SimpleRTK5::rtl812xIdentifyChip(struct srtk5_private *tp) {
    UInt32 reg, val32;
    UInt32 ICVerID;
    bool result = true;

    val32 = RTL_R32(tp, TxConfig);
    reg = val32 & 0x7c800000;
    ICVerID = val32 & 0x00700000;

    tp->chipset = 0xffffffff;
    tp->HwIcVerUnknown = false;

    switch (reg) {
    case 0x60800000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_2;
            tp->chipset = 0;
        } else if (ICVerID == 0x100000) {
            tp->mcfg = CFG_METHOD_3;
            tp->chipset = 1;
        } else {
            tp->mcfg = CFG_METHOD_3;
            tp->chipset = 1;

            tp->HwIcVerUnknown = TRUE;
        }

        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    case 0x64000000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_4;
            tp->chipset = 2;
        } else if (ICVerID == 0x100000) {
            tp->mcfg = CFG_METHOD_5;
            tp->chipset = 3;
        } else {
            tp->mcfg = CFG_METHOD_5;
            tp->chipset = 3;
            tp->HwIcVerUnknown = TRUE;
        }

        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    case 0x64800000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_31;
            tp->chipset = 12;
        } else if (ICVerID == 0x100000) {
            tp->mcfg = CFG_METHOD_32;
            tp->chipset = 13;
        } else if (ICVerID == 0x200000) {
            tp->mcfg = CFG_METHOD_33;
            tp->chipset = 14;
        } else {
            tp->mcfg = CFG_METHOD_31;
            tp->chipset = 12;
            tp->HwIcVerUnknown = TRUE;
        }

        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    case 0x68000000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_8;
            tp->chipset = 6;
        } else if (ICVerID == 0x100000) {
            tp->mcfg = CFG_METHOD_9;
            tp->chipset = 7;
        } else {
            tp->mcfg = CFG_METHOD_9;
            tp->chipset = 7;
            tp->HwIcVerUnknown = TRUE;
        }
        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    case 0x68800000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_10;
            tp->chipset = 8;
        } else if (ICVerID == 0x100000) {
            tp->mcfg = CFG_METHOD_11;
            tp->chipset = 9;
        } else {
            tp->mcfg = CFG_METHOD_11;
            tp->chipset = 9;
            tp->HwIcVerUnknown = TRUE;
        }
        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    case 0x70800000:
        if (ICVerID == 0x00000000) {
            tp->mcfg = CFG_METHOD_12;
            tp->chipset = 10;
        } else {
            tp->mcfg = CFG_METHOD_12;
            tp->chipset = 10;
            tp->HwIcVerUnknown = TRUE;
        }
        // tp->efuse_ver = EFUSE_SUPPORT_V4;
        break;

    default:
        DebugLog("SimpleRTK5: Unknown chip version (%x)\n", reg);
        tp->mcfg = CFG_METHOD_DEFAULT;
        tp->HwIcVerUnknown = TRUE;
        // tp->efuse_ver = EFUSE_NOT_SUPPORT;
        result = false;
        break;
    }

    if (pciDeviceData.device == 0x8162) {
        if (tp->mcfg == CFG_METHOD_3) {
            tp->mcfg = CFG_METHOD_6;
            tp->chipset = 4;
        } else if (tp->mcfg == CFG_METHOD_5) {
            tp->mcfg = CFG_METHOD_7;
            tp->chipset = 5;
        } else if (tp->mcfg == CFG_METHOD_11) {
            tp->mcfg = CFG_METHOD_13;
            tp->chipset = 11;
        }
    }
    this->setProperty(kChipsetName, tp->chipset, 32);
    this->setProperty(kUnknownRevisionName, tp->HwIcVerUnknown);

    tp->srtk5_rx_config = rtlChipInfos[tp->chipset].RCR_Cfg;

#ifdef ENABLE_USE_FIRMWARE_FILE
    tp->fw_name = rtlChipFwInfos[tp->mcfg].fw_name;
#else
    tp->fw_name = NULL;
#endif /* ENABLE_USE_FIRMWARE_FILE */

    return result;
}

void SimpleRTK5::rtl812xInitMacAddr(struct srtk5_private *tp) {
    struct IOEthernetAddress macAddr;
    int i;

    for (i = 0; i < kIOEthernetAddressSize; i++)
        macAddr.bytes[i] = RTL_R8(tp, MAC0 + i);

    *(u32 *)&macAddr.bytes[0] = RTL_R32(tp, BACKUP_ADDR0_8125);
    *(u16 *)&macAddr.bytes[4] = RTL_R16(tp, BACKUP_ADDR1_8125);

    if (is_valid_ether_addr((UInt8 *)&macAddr.bytes))
        goto done;

    if (is_valid_ether_addr((UInt8 *)&fallBackMacAddr.bytes)) {
        memcpy(&macAddr.bytes, &fallBackMacAddr.bytes,
               sizeof(struct IOEthernetAddress));
        goto done;
    }
    /* Create a random Ethernet address. */
    random_buf(&macAddr.bytes, kIOEthernetAddressSize);
    macAddr.bytes[0] &= 0xfe; /* clear multicast bit */
    macAddr.bytes[0] |= 0x02; /* set local assignment bit (IEEE802) */
    DebugLog("SimpleRTK5: Using random MAC address.\n");

done:
    memcpy(&origMacAddr.bytes, &macAddr.bytes,
           sizeof(struct IOEthernetAddress));
    memcpy(&currMacAddr.bytes, &macAddr.bytes,
           sizeof(struct IOEthernetAddress));

    rtl812x_rar_set(&linuxData, (UInt8 *)&currMacAddr.bytes);
}

bool SimpleRTK5::rtl812xInit() {
    struct srtk5_private *tp = &linuxData;
    bool result = false;

    if (!rtl812xIdentifyChip(tp)) {
        IOLog("SimpleRTK5: Unknown chipset. Aborting...\n");
        goto done;
    }
    IOLog("SimpleRTK5: Found %s (chipset %d)\n", rtlChipInfos[tp->chipset].name,
          tp->chipset);

    tp->get_settings = srtk5_gset_xmii;
    tp->phy_reset_enable = srtk5_xmii_reset_enable;
    tp->phy_reset_pending = srtk5_xmii_reset_pending;
    tp->link_ok = srtk5_xmii_link_ok;

    if (!rtl812x_aspm_is_safe(tp)) {
        IOLog("SimpleRTK5: Hardware doesn't support ASPM properly. Disable "
              "it!\n");
        enableASPM = false;
    }
    setupASPM(pciDevice, enableASPM);
    srtk5_init_software_variable(tp, enableASPM);

    /* Setup lpi timer. */
    tp->eee.tx_lpi_timer = mtu + ETH_HLEN + 0x20;

    intrMaskRxTx = (LinkChg | RxDescUnavail | TxOK | RxOK | SWInt);
    intrMaskTimer = (LinkChg | PCSTimeout);
    intrMaskPoll = (LinkChg);
    intrMask = intrMaskRxTx;
    timerValue = 0;

    tp->cp_cmd |= RTL_R16(tp, CPlusCmd);

    srtk5_exit_oob(tp);

    srtk5_powerup_pll(tp);

    rtl812xHwInit(tp);

    srtk5_hw_reset(tp);

    /* Get production from EEPROM */
    srtk5_eeprom_type(tp);

    if (tp->eeprom_type == EEPROM_TYPE_93C46 ||
        tp->eeprom_type == EEPROM_TYPE_93C56)
        srtk5_set_eeprom_sel_low(tp);

    rtl812xInitMacAddr(tp);

    result = true;

done:
    return result;
}

void SimpleRTK5::rtl812xUp(struct srtk5_private *tp) {
    rtl812xHwInit(tp);
    srtk5_hw_reset(tp);
    srtk5_powerup_pll(tp);
    srtk5_hw_ephy_config(tp);
    srtk5_hw_phy_config(tp, enableASPM);
    rtl812xHwConfig(tp);
}

void SimpleRTK5::rtl812xEnable() {
    struct srtk5_private *tp = &linuxData;

    setLinkStatus(kIONetworkLinkValid);

    intrMask = intrMaskRxTx;
    timerValue = 0;
    clear_bit(__POLL_MODE, &stateFlags);

    tp->rms = mtu + VLAN_ETH_HLEN + ETH_FCS_LEN;

    /* restore last modified mac address */
    rtl812x_rar_set(&linuxData, (UInt8 *)&currMacAddr.bytes);
    srtk5_check_hw_phy_mcu_code_ver(tp);

    tp->resume_not_chg_speed = 0;

    if (tp->check_keep_link_speed && srtk5_hw_d3_not_power_off(tp) &&
        srtk5_wait_phy_nway_complete_sleep(tp) == 0)
        tp->resume_not_chg_speed = 1;

    srtk5_exit_oob(tp);
    rtl812xUp(tp);

    rtl812xSetPhyMedium(tp, tp->autoneg, tp->speed, tp->duplex,
                        tp->advertising);

    /* Enable link change interrupt. */
    intrMask = intrMaskRxTx;
    timerValue = 0;
    RTL_W32(tp, IMR0_8125, intrMask);
}

void SimpleRTK5::rtl812xSetOffloadFeatures(bool active) {
    ifnet_t ifnet = netif->getIfnet();
    ifnet_offload_t offload;
    UInt32 mask = 0;

    if (enableTSO4)
        mask |= IFNET_TSO_IPV4;

    if (enableTSO6)
        mask |= IFNET_TSO_IPV6;

    offload = ifnet_offload(ifnet);

    if (active) {
        offload |= mask;
        DebugLog("SimpleRTK5: Enable hardware offload features: %x!\n", mask);
    } else {
        offload &= ~mask;
        DebugLog("SimpleRTK5: Disable hardware offload features: %x!\n", mask);
    }

    if (ifnet_set_offload(ifnet, offload))
        IOLog("SimpleRTK5: Error setting hardware offload: %x!\n", offload);
}

void SimpleRTK5::rtl812xSetMrrs(struct srtk5_private *tp, UInt8 setting) {
    UInt8 devctl;

    devctl = pciDevice->extendedConfigRead8(0x79);
    devctl &= ~0x70;
    devctl |= setting;
    pciDevice->extendedConfigWrite8(0x79, devctl);
}

void SimpleRTK5::rtl812xSetHwFeatures(struct srtk5_private *tp) {
    UInt32 rxcfg = RTL_R32(tp, RxConfig);

    tp->srtk5_rx_config &= ~(AcceptErr | AcceptRunt);
    rxcfg &= ~(AcceptErr | AcceptRunt);

    tp->srtk5_rx_config |= (EnableInnerVlan | EnableOuterVlan);
    rxcfg |= (EnableInnerVlan | EnableOuterVlan);

    RTL_W32(tp, RxConfig, rxcfg);

    tp->cp_cmd |= RxChkSum;

    RTL_W16(tp, CPlusCmd, tp->cp_cmd);
    RTL_R16(tp, CPlusCmd);
}

void SimpleRTK5::rtl812xHwInit(struct srtk5_private *tp) {
    u32 csi_tmp;

    srtk5_enable_aspm_clkreq_lock(tp, 0);
    srtk5_enable_force_clkreq(tp, 0);

    srtk5_set_reg_oobs_en_sel(tp, true);

    // Disable UPS
    srtk5_mac_ocp_write(tp, 0xD40A, srtk5_mac_ocp_read(tp, 0xD40A) & ~(BIT_4));

#ifndef ENABLE_USE_FIRMWARE_FILE
    srtk5_hw_mac_mcu_config(tp);
#endif

    /*disable ocp phy power saving*/
    if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3 ||
        tp->mcfg == CFG_METHOD_6)
        srtk5_disable_ocp_phy_power_saving(tp);

    // Set PCIE uncorrectable error status mask pcie 0x108
    csi_tmp = srtk5_csi_read(tp, 0x108);
    csi_tmp |= BIT_20;
    srtk5_csi_write(tp, 0x108, csi_tmp);

    srtk5_enable_cfg9346_write(tp);
    srtk5_disable_linkchg_wakeup(tp);
    srtk5_disable_cfg9346_write(tp);
    srtk5_disable_magic_packet(tp);
    srtk5_disable_d0_speedup(tp);
    // srtk5_set_pci_pme(tp, 0);

    srtk5_enable_magic_packet(tp);

#ifdef ENABLE_USE_FIRMWARE_FILE
    if (tp->rtl_fw && !tp->resume_not_chg_speed)
        srtk5_apply_firmware(tp);
#endif
}

void SimpleRTK5::rtl812xDown(struct srtk5_private *tp) {
    srtk5_irq_mask_and_ack(tp);
    srtk5_hw_reset(tp);
    clearRxTxRings();
}

void SimpleRTK5::rtl812xDisable() {
    struct srtk5_private *tp = &linuxData;

    rtl812xDown(tp);
    srtk5_hw_d3_para(tp);
    srtk5_powerdown_pll(tp);

    if (HW_DASH_SUPPORT_DASH(tp))
        srtk5_driver_stop(tp);
}

void SimpleRTK5::rtl812xRestart(struct srtk5_private *tp) {
    /* Stop output thread and flush txQueue */
    netif->stopOutputThread();
    netif->flushOutputQueue();

    clear_bit(__LINK_UP, &stateFlags);
    setLinkStatus(kIONetworkLinkValid);

    /* Reset NIC and cleanup both descriptor rings. */
    srtk5_hw_reset(tp);

    clearRxTxRings();

    /* Reinitialize NIC. */
    rtl812xEnable();
}

void SimpleRTK5::rtl812xHwConfig(struct srtk5_private *tp) {
    UInt16 mac_ocp_data;

    srtk5_disable_rx_packet_filter(tp);

    srtk5_hw_reset(tp);

    srtk5_enable_cfg9346_write(tp);

    srtk5_enable_force_clkreq(tp, 0);
    srtk5_enable_aspm_clkreq_lock(tp, 0);

    srtk5_set_eee_lpi_timer(tp);

    // keep magic packet only
    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xC0B6);
    mac_ocp_data &= BIT_0;
    srtk5_mac_ocp_write(tp, 0xC0B6, mac_ocp_data);

    /* Fill tally counter address. */
    RTL_W32(tp, CounterAddrHigh, (statPhyAddr >> 32));
    RTL_W32(tp, CounterAddrLow, (statPhyAddr & 0x00000000ffffffff));

    /* Enable extended tally counter. */
    srtk5_set_mac_ocp_bit(tp, 0xEA84, (BIT_1 | BIT_0));

    /* Setup the descriptor rings. */
#ifdef ENABLE_TX_NO_CLOSE
    txTailPtr0 = txClosePtr0 = 0;
#endif

    txNextDescIndex = txDirtyDescIndex = 0;
    txNumFreeDesc = kNumTxDesc;
    rxNextDescIndex = 0;

    RTL_W32(tp, TxDescStartAddrLow, (txPhyAddr & 0x00000000ffffffff));
    RTL_W32(tp, TxDescStartAddrHigh, (txPhyAddr >> 32));
    RTL_W32(tp, RxDescAddrLow, (rxPhyAddr & 0x00000000ffffffff));
    RTL_W32(tp, RxDescAddrHigh, (rxPhyAddr >> 32));

    /* Set DMA burst size and Interframe Gap Time */
    RTL_W32(tp, TxConfig,
            (TX_DMA_BURST_unlimited << TxDMAShift) |
                (InterFrameGap << TxInterFrameGapShift));

#ifdef ENABLE_TX_NO_CLOSE
    /* Enable TxNoClose. */
    RTL_W32(tp, TxConfig, (RTL_R32(tp, TxConfig) | BIT_6));
#endif

    /* Disable double VLAN. */
    RTL_W16(tp, DOUBLE_VLAN_CONFIG, 0);

    switch (tp->mcfg) {
    case CFG_METHOD_2 ... CFG_METHOD_7:
        srtk5_enable_tcam(tp);
        break;
    }

    srtk5_set_l1_l0s_entry_latency(tp);

    rtl812xSetMrrs(tp, 0x40);

    RTL_W32(tp, RSS_CTRL_8125, 0x00);

    RTL_W16(tp, Q_NUM_CTRL_8125, 0);

    RTL_W8(tp, Config1, RTL_R8(tp, Config1) & ~0x10);

    srtk5_mac_ocp_write(tp, 0xC140, 0xFFFF);
    srtk5_mac_ocp_write(tp, 0xC142, 0xFFFF);

    /*
     * Disabling the new tx descriptor format seems to prevent
     * tx timeouts when using TSO.
     */
    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xEB58);

#ifdef USE_NEW_TX_DESC
    mac_ocp_data |= (BIT_0);
#else
    mac_ocp_data &= ~(BIT_0);
#endif /* USE_NEW_TX_DESC */

    srtk5_mac_ocp_write(tp, 0xEB58, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xE614);
    mac_ocp_data &= ~(BIT_10 | BIT_9 | BIT_8);

    if (tp->mcfg == CFG_METHOD_4 || tp->mcfg == CFG_METHOD_5 ||
        tp->mcfg == CFG_METHOD_7)
        mac_ocp_data |= ((2 & 0x07) << 8);
    else
        mac_ocp_data |= ((3 & 0x07) << 8);

    srtk5_mac_ocp_write(tp, 0xE614, mac_ocp_data);

    /* Set number of tx queues to 1. */
    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xE63E);
    mac_ocp_data &= ~(BIT_11 | BIT_10);
    srtk5_mac_ocp_write(tp, 0xE63E, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xE63E);
    mac_ocp_data &= ~(BIT_5 | BIT_4);
    mac_ocp_data |= (0x02 << 4);
    srtk5_mac_ocp_write(tp, 0xE63E, mac_ocp_data);

    srtk5_enable_mcu(tp, 0);
    srtk5_enable_mcu(tp, 1);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xC0B4);
    mac_ocp_data |= (BIT_3 | BIT_2);
    srtk5_mac_ocp_write(tp, 0xC0B4, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xEB6A);
    mac_ocp_data &=
        ~(BIT_7 | BIT_6 | BIT_5 | BIT_4 | BIT_3 | BIT_2 | BIT_1 | BIT_0);
    mac_ocp_data |= (BIT_5 | BIT_4 | BIT_1 | BIT_0);
    srtk5_mac_ocp_write(tp, 0xEB6A, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xEB50);
    mac_ocp_data &= ~(BIT_9 | BIT_8 | BIT_7 | BIT_6 | BIT_5);
    mac_ocp_data |= (BIT_6);
    srtk5_mac_ocp_write(tp, 0xEB50, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xE056);
    mac_ocp_data &= ~(BIT_7 | BIT_6 | BIT_5 | BIT_4);
    // mac_ocp_data |= (BIT_4 | BIT_5);
    srtk5_mac_ocp_write(tp, 0xE056, mac_ocp_data);

    RTL_W8(tp, TDFNR, 0x10);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xE040);
    mac_ocp_data &= ~(BIT_12);
    srtk5_mac_ocp_write(tp, 0xE040, mac_ocp_data);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xEA1C);
    mac_ocp_data &= ~(BIT_1 | BIT_0);
    mac_ocp_data |= (BIT_0);
    srtk5_mac_ocp_write(tp, 0xEA1C, mac_ocp_data);

    switch (tp->mcfg) {
    case CFG_METHOD_2:
    case CFG_METHOD_3:
    case CFG_METHOD_6:
    case CFG_METHOD_8:
    case CFG_METHOD_9:
    case CFG_METHOD_12:
        srtk5_oob_mutex_lock(tp);
        break;
    }

    if (tp->mcfg == CFG_METHOD_10 || tp->mcfg == CFG_METHOD_11 ||
        tp->mcfg == CFG_METHOD_13)
        srtk5_mac_ocp_write(tp, 0xE0C0, 0x4403);
    else
        srtk5_mac_ocp_write(tp, 0xE0C0, 0x4000);

    srtk5_set_mac_ocp_bit(tp, 0xE052, (BIT_6 | BIT_5));
    srtk5_clear_mac_ocp_bit(tp, 0xE052, BIT_3 | BIT_7);

    switch (tp->mcfg) {
    case CFG_METHOD_2:
    case CFG_METHOD_3:
    case CFG_METHOD_6:
    case CFG_METHOD_8:
    case CFG_METHOD_9:
    case CFG_METHOD_12:
        srtk5_oob_mutex_unlock(tp);
        break;
    }

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xD430);
    mac_ocp_data &= ~(BIT_11 | BIT_10 | BIT_9 | BIT_8 | BIT_7 | BIT_6 | BIT_5 |
                      BIT_4 | BIT_3 | BIT_2 | BIT_1 | BIT_0);
    mac_ocp_data |= 0x45F;
    srtk5_mac_ocp_write(tp, 0xD430, mac_ocp_data);

    // srtk5_mac_ocp_write(tp, 0xE0C0, 0x4F87);
    if (!tp->DASH)
        RTL_W8(tp, 0xD0, RTL_R8(tp, 0xD0) | BIT_6 | BIT_7);
    else
        RTL_W8(tp, 0xD0, RTL_R8(tp, 0xD0) & ~(BIT_6 | BIT_7));

    if (tp->mcfg == CFG_METHOD_2 || tp->mcfg == CFG_METHOD_3 ||
        tp->mcfg == CFG_METHOD_6)
        RTL_W8(tp, MCUCmd_reg, RTL_R8(tp, MCUCmd_reg) | BIT_0);

    if (tp->mcfg != CFG_METHOD_10 && tp->mcfg != CFG_METHOD_11 &&
        tp->mcfg != CFG_METHOD_13)
        srtk5_disable_eee_plus(tp);

    mac_ocp_data = srtk5_mac_ocp_read(tp, 0xEA1C);
    mac_ocp_data &= ~(BIT_2);
    srtk5_mac_ocp_write(tp, 0xEA1C, mac_ocp_data);

    srtk5_clear_tcam_entries(tp);

    RTL_W16(tp, 0x1880, RTL_R16(tp, 0x1880) & ~(BIT_4 | BIT_5));

    if (tp->HwSuppRxDescType == RX_DESC_RING_TYPE_4) {
        RTL_W8(tp, 0xd8, RTL_R8(tp, 0xd8) & ~EnableRxDescV4_0);
    }

    if (tp->mcfg == CFG_METHOD_12) {
        srtk5_clear_mac_ocp_bit(tp, 0xE00C, BIT_12);
        srtk5_clear_mac_ocp_bit(tp, 0xC0C2, BIT_6);
    }

    // other hw parameters
    srtk5_hw_clear_timer_int(tp);

    srtk5_hw_clear_int_miti(tp);

    srtk5_enable_exit_l1_mask(tp);

    srtk5_mac_ocp_write(tp, 0xE098, 0xC302);

    if (enableASPM && (tp->org_pci_offset_99 & (BIT_2 | BIT_5 | BIT_6)))
        srtk5_init_pci_offset_99(tp);
    else
        srtk5_disable_pci_offset_99(tp);

    if (enableASPM && (tp->org_pci_offset_180 & srtk5_get_l1off_cap_bits(tp)))
        srtk5_init_pci_offset_180(tp);
    else
        srtk5_disable_pci_offset_180(tp);

    if (tp->RequiredPfmPatch)
        srtk5_set_pfm_patch(tp, 0);

    tp->cp_cmd &= ~(EnableBist | Macdbgo_oe | Force_halfdup | Force_rxflow_en |
                    Force_txflow_en | Cxpl_dbg_sel | ASF | Macdbgo_sel);

    rtl812xSetHwFeatures(tp);

    srtk5_set_rms(tp, tp->rms);

    srtk5_disable_rxdvgate(tp);

    /* Set Rx packet filter */
    // srtk5_hw_set_rx_packet_filter(dev);
    /* Set receiver mode. */
    setMulticastMode(test_bit(__M_CAST, &stateFlags));

    srtk5_enable_aspm_clkreq_lock(tp, enableASPM ? 1 : 0);

    srtk5_disable_cfg9346_write(tp);

    udelay(10);
}

#ifdef ENABLE_TX_NO_CLOSE
UInt32 SimpleRTK5::rtl812xGetHwCloPtr(struct srtk5_private *tp) {
    UInt32 cloPtr;

    if (tp->HwSuppTxNoCloseVer == 3)
        cloPtr = RTL_R16(tp, tp->HwCloPtrReg);
    else
        cloPtr = RTL_R32(tp, tp->HwCloPtrReg);

    return cloPtr;
}

void SimpleRTK5::rtl812xDoorbell(struct srtk5_private *tp, UInt32 txTailPtr) {
    if (tp->HwSuppTxNoCloseVer > 3)
        RTL_W32(tp, tp->SwTailPtrReg, txTailPtr);
    else
        RTL_W16(tp, tp->SwTailPtrReg, txTailPtr & 0xffff);
}
#endif

void SimpleRTK5::getChecksumResult(mbuf_t m, UInt32 status1, UInt32 status2) {
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

UInt32 SimpleRTK5::updateTimerValue(struct srtk5_private *tp, UInt32 status) {
    UInt32 newTimerValue = 0;

    if (status & (RxOK | TxOK)) {
        if (tp->speed < SPEED_1000) {
            newTimerValue = kTimerBulk;
            goto done;
        }
        if (mtu > MSS_MAX) {
            if ((totalDescs > 96) || (totalDescs < 4))
                newTimerValue = kTimerLat2;
            else
                newTimerValue = kTimerDefault;

            goto done;
        }
        if ((totalDescs > 4) && (totalBytes / totalDescs) > 2000) {
            newTimerValue = kTimerBulk;
            goto done;
        }
        if ((totalDescs > 35) || (totalBytes < 1500)) {
            newTimerValue = kTimerLat1;
            goto done;
        }
        newTimerValue = kTimerDefault;
    }

done:
#ifdef DEBUG_INTR
    if (status & PCSTimeout)
        tmrInterrupts++;

    if (totalDescs > maxTxPkt) {
        maxTxPkt = totalDescs;
    }
#endif

clear_count:
    totalDescs = 0;
    totalBytes = 0;

    return newTimerValue;
}

#pragma mark--- link management methods ---

void SimpleRTK5::rtl812xLinkOnPatch(struct srtk5_private *tp) {
    UInt32 status;

    rtl812xHwConfig(tp);

    if (tp->mcfg == CFG_METHOD_2) {
        if (srtk5_get_phy_status(tp) & FullDup)
            RTL_W32(tp, TxConfig,
                    (RTL_R32(tp, TxConfig) | (BIT_24 | BIT_25)) & ~BIT_19);
        else
            RTL_W32(tp, TxConfig,
                    (RTL_R32(tp, TxConfig) | BIT_25) & ~(BIT_19 | BIT_24));
    }

    status = srtk5_get_phy_status(tp);

    switch (tp->mcfg) {
    case CFG_METHOD_2:
    case CFG_METHOD_3:
    case CFG_METHOD_4:
    case CFG_METHOD_5:
    case CFG_METHOD_6:
    case CFG_METHOD_7:
    case CFG_METHOD_8:
    case CFG_METHOD_9:
    case CFG_METHOD_12:
    case CFG_METHOD_31:
    case CFG_METHOD_32:
    case CFG_METHOD_33:
        if (status & _10bps)
            srtk5_enable_eee_plus(tp);
        break;

    default:
        break;
    }

    if (tp->RequiredPfmPatch)
        srtk5_set_pfm_patch(tp, (status & _10bps) ? 1 : 0);

    tp->phy_reg_aner = srtk5_mdio_read(tp, MII_EXPANSION);
    tp->phy_reg_anlpar = srtk5_mdio_read(tp, MII_LPA);
    tp->phy_reg_gbsr = srtk5_mdio_read(tp, MII_STAT1000);
    tp->phy_reg_status_2500 = srtk5_mdio_direct_read_phy_ocp(tp, 0xA5D6);
}

void SimpleRTK5::rtl812xLinkDownPatch(struct srtk5_private *tp) {
    tp->phy_reg_aner = 0;
    tp->phy_reg_anlpar = 0;
    tp->phy_reg_gbsr = 0;
    tp->phy_reg_status_2500 = 0;

    switch (tp->mcfg) {
    case CFG_METHOD_2:
    case CFG_METHOD_3:
    case CFG_METHOD_4:
    case CFG_METHOD_5:
    case CFG_METHOD_6:
    case CFG_METHOD_7:
    case CFG_METHOD_8:
    case CFG_METHOD_9:
    case CFG_METHOD_12:
    case CFG_METHOD_31:
    case CFG_METHOD_32:
    case CFG_METHOD_33:
        srtk5_disable_eee_plus(tp);
        break;

    default:
        break;
    }
    if (tp->RequiredPfmPatch)
        srtk5_set_pfm_patch(tp, 1);

    srtk5_hw_reset(tp);
}

void SimpleRTK5::rtl812xGetEEEMode(struct srtk5_private *tp) {
    UInt32 adv, lp, sup;
    UInt16 val;

    /* Get supported EEE. */
    // val = srtk5_mdio_direct_read_phy_ocp(tp, 0xA5C4);
    // sup = mmd_eee_cap_to_ethtool_sup_t(val);
    sup = tp->eee.supported;
    DebugLog("SimpleRTK5: EEE supported: %u\n", sup);

    /* Get advertisement EEE */
    val = srtk5_mdio_direct_read_phy_ocp(tp, 0xA5D0);
    adv = mmd_eee_adv_to_ethtool_adv_t(val);
    val = srtk5_mdio_direct_read_phy_ocp(tp, 0xA6D4);

    if (val & RTK_EEE_ADVERTISE_2500FULL)
        adv |= ADVERTISED_2500baseX_Full;

    DebugLog("SimpleRTK5: EEE advertised: %u\n", adv);

    /* Get LP advertisement EEE */
    val = srtk5_mdio_direct_read_phy_ocp(tp, 0xA5D2);
    lp = mmd_eee_adv_to_ethtool_adv_t(val);
    DebugLog("SimpleRTK5: EEE link partner: %u\n", lp);

    val = srtk5_mdio_direct_read_phy_ocp(tp, 0xA6D0);

    if (val & RTK_LPA_EEE_ADVERTISE_2500FULL)
        lp |= ADVERTISED_2500baseX_Full;

    val = srtk5_mac_ocp_read(tp, 0xE040);
    val &= BIT_1 | BIT_0;

    tp->eee.eee_enabled = !!val;
    tp->eee.eee_active = !!(sup & adv & lp);
}

void SimpleRTK5::rtl812xCheckLinkStatus(struct srtk5_private *tp) {
    UInt32 status;

    status = RTL_R32(tp, PHYstatus);

    if ((status == 0xffffffff) || !(status & LinkStatus)) {
        rtl812xLinkDownPatch(tp);

        /* Stop watchdog and statistics updates. */
        timerSource->cancelTimeout();
        setLinkDown();

        clearRxTxRings();
    } else {
        /* Get EEE mode. */
        rtl812xGetEEEMode(tp);

        /* Get link speed, duplex and flow-control mode. */
        if (status & (TxFlowCtrl | RxFlowCtrl)) {
            tp->fcpause = srtk5_fc_full;
        } else {
            tp->fcpause = srtk5_fc_none;
        }
        if (status & _2500bpsF) {
            tp->speed = SPEED_2500;
            tp->duplex = DUPLEX_FULL;
        } else if (status & _1000bpsF) {
            tp->speed = SPEED_1000;
            tp->duplex = DUPLEX_FULL;
        } else if (status & _100bps) {
            tp->speed = SPEED_100;

            if (status & FullDup) {
                tp->duplex = DUPLEX_FULL;
            } else {
                tp->duplex = DUPLEX_HALF;
            }
        } else {
            tp->speed = SPEED_10;

            if (status & FullDup) {
                tp->duplex = DUPLEX_FULL;
            } else {
                tp->duplex = DUPLEX_HALF;
            }
        }
        rtl812xLinkOnPatch(tp);

        setLinkUp();
        timerSource->setTimeoutMS(kTimeoutMS);
    }
}

void SimpleRTK5::setLinkUp() {
    struct srtk5_private *tp = &linuxData;
    const char *speedName;
    const char *duplexName;
    const char *flowName;
    const char *eeeName;
    UInt64 mediumSpeed;
    UInt32 mediumIndex = MIDX_AUTO;
    UInt32 spd = tp->speed;
    UInt32 fc = tp->fcpause;
    bool eee;

    totalDescs = 0;
    totalBytes = 0;

    eee = tp->eee.eee_active;
    eeeName = eeeNames[kEEETypeNo];

    /* Get link speed, duplex and flow-control mode. */
    if (fc == srtk5_fc_full) {
        flowName = onFlowName;
    } else {
        flowName = offFlowName;
    }
    if (spd == SPEED_5000) {
        mediumSpeed = kSpeed5000MBit;
        speedName = speed5GName;
        duplexName = duplexFullName;

        if (fc == srtk5_fc_full) {
            if (eee) {
                mediumIndex = MIDX_5000FDFC_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_5000FDFC;
                eeeName = eeeNames[kEEETypeNo];
            }
        } else {
            if (eee) {
                mediumIndex = MIDX_5000FD_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_5000FD;
                eeeName = eeeNames[kEEETypeNo];
            }
        }
    } else if (spd == SPEED_2500) {
        mediumSpeed = kSpeed2500MBit;
        speedName = speed25GName;
        duplexName = duplexFullName;

        if (fc == srtk5_fc_full) {
            if (eee) {
                mediumIndex = MIDX_2500FDFC_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_2500FDFC;
                eeeName = eeeNames[kEEETypeNo];
            }
        } else {
            if (eee) {
                mediumIndex = MIDX_2500FD_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_2500FD;
                eeeName = eeeNames[kEEETypeNo];
            }
        }
    } else if (spd == SPEED_1000) {
        mediumSpeed = kSpeed1000MBit;
        speedName = speed1GName;
        duplexName = duplexFullName;

        if (fc == srtk5_fc_full) {
            if (eee) {
                mediumIndex = MIDX_1000FDFC_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_1000FDFC;
            }
        } else {
            if (eee) {
                mediumIndex = MIDX_1000FD_EEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MIDX_1000FD;
            }
        }
    } else if (spd == SPEED_100) {
        mediumSpeed = kSpeed100MBit;
        speedName = speed100MName;

        if (tp->duplex == DUPLEX_FULL) {
            duplexName = duplexFullName;

            if (fc == srtk5_fc_full) {
                if (eee) {
                    mediumIndex = MIDX_100FDFC_EEE;
                    eeeName = eeeNames[kEEETypeYes];
                } else {
                    mediumIndex = MIDX_100FDFC;
                }
            } else {
                if (eee) {
                    mediumIndex = MIDX_100FD_EEE;
                    eeeName = eeeNames[kEEETypeYes];
                } else {
                    mediumIndex = MIDX_100FD;
                }
            }
        } else {
            mediumIndex = MIDX_100HD;
            duplexName = duplexHalfName;
        }
    } else {
        mediumSpeed = kSpeed10MBit;
        speedName = speed10MName;

        if (tp->duplex == DUPLEX_FULL) {
            mediumIndex = MIDX_10FD;
            duplexName = duplexFullName;
        } else {
            mediumIndex = MIDX_10HD;
            duplexName = duplexHalfName;
        }
    }
    rxPacketHead = rxPacketTail = NULL;
    rxPacketSize = 0;

    /* Start hardware. */
    RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);

    set_bit(__LINK_UP, &stateFlags);
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive,
                  mediumTable[mediumIndex], mediumSpeed, NULL);

    /* Start output thread, statistics update and watchdog. Also
     * update poll params according to link speed.
     */
    bzero(&pollParms, sizeof(IONetworkPacketPollingParameters));

    if (spd == SPEED_10) {
        pollParms.lowThresholdPackets = 2;
        pollParms.highThresholdPackets = 8;
        pollParms.lowThresholdBytes = 0x400;
        pollParms.highThresholdBytes = 0x1800;
        pollParms.pollIntervalTime = 1000000; /* 1ms */
    } else {
        pollParms.lowThresholdPackets = 10;
        pollParms.highThresholdPackets = 40;
        pollParms.lowThresholdBytes = 0x1000;
        pollParms.highThresholdBytes = 0x10000;

        if (spd == SPEED_5000)
            pollParms.pollIntervalTime = pollTime5G;
        else if (spd == SPEED_2500)
            pollParms.pollIntervalTime = pollTime2G;
        else if (spd == SPEED_1000)
            pollParms.pollIntervalTime = 170000; /* 170µs */
        else
            pollParms.pollIntervalTime = 1000000; /* 1ms */
    }
    netif->setPacketPollingParameters(&pollParms, 0);
    DebugLog("SimpleRTK5: pollIntervalTime: %lluµs\n",
             (pollParms.pollIntervalTime / 1000));

    netif->startOutputThread();

    IOLog("SimpleRTK5: Link up on en%u, %s, %s, %s%s\n", netif->getUnitNumber(),
          speedName, duplexName, flowName, eeeName);
}

void SimpleRTK5::setLinkDown() {
    struct srtk5_private *tp = &linuxData;

    deadlockWarn = 0;

    /* Stop output thread and flush output queue. */
    netif->stopOutputThread();
    netif->flushOutputQueue();

    /* Update link status. */
    clear_mask((__LINK_UP_M | __POLL_MODE_M), &stateFlags);
    setLinkStatus(kIONetworkLinkValid);

    rtl812xLinkDownPatch(tp);
    clearRxTxRings();

    /* Enable link change interrupt. */
    intrMask = intrMaskRxTx;
    timerValue = 0;
    RTL_W32(tp, IMR0_8125, intrMask);

    rtl812xSetPhyMedium(tp, tp->autoneg, tp->speed, tp->duplex,
                        tp->advertising);

    IOLog("SimpleRTK5: Link down on en%u\n", netif->getUnitNumber());
}

void SimpleRTK5::rtl812xSetPhyMedium(struct srtk5_private *tp, UInt8 autoneg,
                                     UInt32 speed, UInt8 duplex, UInt64 adv) {
    int auto_nego = 0;
    int giga_ctrl = 0;
    int ctrl_2500 = 0;

    DebugLog("SimpleRTK5: speed: %u, duplex: %u, adv: %llx\n",
             static_cast<unsigned int>(speed), duplex, adv);

    if (!srtk5_is_speed_mode_valid(speed)) {
        speed = SPEED_2500;
        duplex = DUPLEX_FULL;
        adv |= tp->advertising;
    }

    /* Enable or disable EEE support according to selected medium. */
    if (tp->eee.eee_enabled && (autoneg == AUTONEG_ENABLE)) {
        srtk5_enable_eee(tp);
        DebugLog("SimpleRTK5: Enable EEE support.\n");
    } else {
        srtk5_disable_eee(tp);
        DebugLog("SimpleRTK5: Disable EEE support.\n");
    }
    if (enableGigaLite && (autoneg == AUTONEG_ENABLE))
        srtk5_enable_giga_lite(tp, adv);
    else
        srtk5_disable_giga_lite(tp);

    giga_ctrl = srtk5_mdio_read(tp, MII_CTRL1000);
    giga_ctrl &= ~(ADVERTISE_1000HALF | ADVERTISE_1000FULL);
    ctrl_2500 = srtk5_mdio_direct_read_phy_ocp(tp, 0xA5D4);
    ctrl_2500 &= ~RTK_ADVERTISE_2500FULL;

    if (autoneg == AUTONEG_ENABLE) {
        /*n-way force*/
        auto_nego = srtk5_mdio_read(tp, MII_ADVERTISE);
        auto_nego &=
            ~(ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF |
              ADVERTISE_100FULL | ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);

        if (adv & ADVERTISED_10baseT_Half)
            auto_nego |= ADVERTISE_10HALF;
        if (adv & ADVERTISED_10baseT_Full)
            auto_nego |= ADVERTISE_10FULL;
        if (adv & ADVERTISED_100baseT_Half)
            auto_nego |= ADVERTISE_100HALF;
        if (adv & ADVERTISED_100baseT_Full)
            auto_nego |= ADVERTISE_100FULL;
        if (adv & ADVERTISED_1000baseT_Half)
            giga_ctrl |= ADVERTISE_1000HALF;
        if (adv & ADVERTISED_1000baseT_Full)
            giga_ctrl |= ADVERTISE_1000FULL;
        if (adv & ADVERTISED_2500baseX_Full)
            ctrl_2500 |= RTK_ADVERTISE_2500FULL;

        // flow control
        if (tp->fcpause == srtk5_fc_full)
            auto_nego |= ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;

        tp->phy_auto_nego_reg = auto_nego;
        tp->phy_1000_ctrl_reg = giga_ctrl;

        tp->phy_2500_ctrl_reg = ctrl_2500;

        srtk5_mdio_write(tp, 0x1f, 0x0000);
        srtk5_mdio_write(tp, MII_ADVERTISE, auto_nego);
        srtk5_mdio_write(tp, MII_CTRL1000, giga_ctrl);
        srtk5_mdio_direct_write_phy_ocp(tp, 0xA5D4, ctrl_2500);
        srtk5_phy_restart_nway(tp);
    } else {
        /*true force*/
        if (speed == SPEED_10 || speed == SPEED_100)
            srtk5_phy_setup_force_mode(tp, speed, duplex);
        else
            return;
    }
    tp->autoneg = autoneg;
    tp->speed = speed;
    tp->duplex = duplex;
    tp->advertising = adv;

    srtk5_set_d0_speedup_speed(tp);
}

#pragma mark--- statistics update methods ---

void SimpleRTK5::rtl812xDumpTallyCounter(struct srtk5_private *tp) {
    UInt32 cmd;

    RTL_W32(tp, CounterAddrHigh, (statPhyAddr >> 32));
    cmd = statPhyAddr & 0x00000000ffffffff;
    RTL_W32(tp, CounterAddrLow, cmd);
    RTL_W32(tp, CounterAddrLow, cmd | CounterDump);
}

void SimpleRTK5::runStatUpdateThread(thread_call_param_t param0) {
    ((SimpleRTK5 *)param0)->statUpdateThread();
}

/*
 * Perform delayed mapping of a defined number of batches
 * and set the ring state to indicate, that mapping is
 * in progress.
 */
void SimpleRTK5::statUpdateThread() {
    struct srtk5_private *tp = &linuxData;
    UInt32 sgColl, mlColl;

    if (!(RTL_R32(tp, CounterAddrLow) & CounterDump)) {
        netStats->inputPackets =
            OSSwapLittleToHostInt64(statData->rxPackets) & 0x00000000ffffffff;
        netStats->inputErrors = OSSwapLittleToHostInt32(statData->rxErrors);
        netStats->outputPackets =
            OSSwapLittleToHostInt64(statData->txPackets) & 0x00000000ffffffff;
        netStats->outputErrors = OSSwapLittleToHostInt32(statData->txErrors);

        sgColl = OSSwapLittleToHostInt32(statData->txOneCollision);
        mlColl = OSSwapLittleToHostInt32(statData->txMultiCollision);
        netStats->collisions = sgColl + mlColl;

        etherStats->dot3StatsEntry.singleCollisionFrames = sgColl;
        etherStats->dot3StatsEntry.multipleCollisionFrames = mlColl;
        etherStats->dot3StatsEntry.alignmentErrors =
            OSSwapLittleToHostInt16(statData->alignErrors);
        etherStats->dot3StatsEntry.missedFrames =
            OSSwapLittleToHostInt16(statData->rxMissed);
        etherStats->dot3TxExtraEntry.underruns =
            OSSwapLittleToHostInt16(statData->txUnderun);
    }
}

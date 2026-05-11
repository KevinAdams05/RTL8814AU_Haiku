/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * RTL8814AU — Master header for the Realtek RTL8814AU native USB WiFi driver.
 *
 * Contains hardware register addresses, bit definitions, descriptor formats,
 * USB device IDs, and shared constants. All register addresses are sourced from
 * the reference driver (ulli-kroll/rtl8814au, include/rtl8814a_spec.h).
 *
 * Register I/O: All registers are accessed via USB vendor control transfers.
 * There is no memory-mapped I/O on USB devices. See RegisterIO.h for the
 * read/write interface.
 */
#ifndef RTL8814AU_H
#define RTL8814AU_H


#include <SupportDefs.h>


// ---------------------------------------------------------------------------
// Driver identity
// ---------------------------------------------------------------------------

#define RTL8814AU_DRIVER_NAME		"rtl8814au"
#define RTL8814AU_DEVICE_PATH_BASE	"net/" RTL8814AU_DRIVER_NAME

// Maximum number of simultaneously attached adapters
static const uint32 kMaxDeviceCount = 8;


// ---------------------------------------------------------------------------
// USB device IDs
// ---------------------------------------------------------------------------

struct RTL8814AUDeviceID {
	uint16	vendorID;
	uint16	productID;
	const char*	name;
};

// Devices known to use the RTL8814AU chipset. This table is checked during
// USB device_added() to decide whether we claim a device.
static const RTL8814AUDeviceID kSupportedDevices[] = {
	{ 0x0b05, 0x1817, "ASUS USB-AC68" },
	{ 0x7392, 0xa833, "Edimax AC1750" },
	{ 0x0b05, 0x1852, "ASUS USB-AC68 (rev 2)" },
	{ 0x0846, 0x9054, "Netgear A7000" },
	{ 0x2001, 0x331a, "D-Link DWA-192" },
	{ 0x2357, 0x0106, "TP-Link Archer T9UH" },
	{ 0x20f4, 0x809a, "TRENDnet TEW-809UB" },
	{ 0x056e, 0x400b, "Elecom WDB-867DU3S" },
	{ 0x056e, 0x400d, "Elecom WDC-867DU3S" },
};

static const uint32 kSupportedDeviceCount
	= sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]);


// ---------------------------------------------------------------------------
// Chip identification
// ---------------------------------------------------------------------------

static const uint32 kRTL8814AU_ChipID		= 0x8814;
static const uint32 kMaxFirmwareSize		= 0x18000;	// 96 KB

// TX buffer: 2048 pages of 128 bytes each = 256 KB total
static const uint32 kTxPageSize				= 128;
static const uint32 kTxPageCount			= 2048;
static const uint32 kTxBufferSize			= kTxPageSize * kTxPageCount;

// RX buffer size in hardware
static const uint32 kRxBufferSize			= 0x5C00;	// ~23 KB

// Station tracking and security
static const uint32 kMaxMacIDCount			= 128;
static const uint32 kMaxSecurityCamEntries	= 64;

// H2C mailbox system
static const uint32 kH2CMailboxCount		= 4;
static const uint32 kH2CCommandSize		= 7;		// 4 standard + 3 ext

// EFUSE
static const uint32 kEfuseTotalSize			= 1024;
static const uint32 kEfuseMapSize			= 512;

// RF paths — the RTL8814AU has 4 independent radio chains
static const uint32 kRfPathCount			= 4;

// AMPDU parameters
static const uint8 kAmpduMaxTime			= 0x70;
static const uint32 kAmpduMaxLength			= 0x1FFFF;	// 128 KB

// Firmware path on Haiku filesystem
#define RTL8814AU_FIRMWARE_PATH \
	"/boot/system/data/firmware/rtl8814au/rtl8814aufw.bin"


// ---------------------------------------------------------------------------
// USB endpoint configuration
//
// The RTL8814AU exposes 5 endpoints:
//   3 bulk OUT (TX, priority-mapped)
//   1 bulk IN  (RX data + status)
//   1 interrupt IN (C2H firmware events)
// ---------------------------------------------------------------------------

static const uint32 kBulkOutEndpointCount	= 3;
static const uint32 kBulkInEndpointCount	= 1;
static const uint32 kInterruptInEndpointCount = 1;

// USB bulk transfer buffer sizes
static const uint32 kUsbTxBufferSize		= 16384;	// 16 KB per bulk OUT
static const uint32 kUsbRxBufferSize		= 32768;	// 32 KB for bulk IN
static const uint32 kUsbInterruptBufferSize	= 64;		// for interrupt IN

// Queue-to-endpoint mapping (WMM priority → bulk OUT pipe index)
enum TxQueueSelect {
	kTxQueueVO		= 0,	// Voice — highest priority → OUT #1
	kTxQueueVI		= 0,	// Video — high priority → OUT #1
	kTxQueueBE		= 1,	// Best effort — normal → OUT #2
	kTxQueueBK		= 1,	// Background — low priority → OUT #2
	kTxQueueMGT		= 2,	// Management → OUT #3
	kTxQueueCMD		= 2,	// H2C commands → OUT #3
	kTxQueueBCN		= 2,	// Beacons → OUT #3
	kTxQueueHIGH	= 0,	// High priority → OUT #1
};


// ---------------------------------------------------------------------------
// Register addresses — System Configuration (0x0000 – 0x00FF)
//
// Power control, clock generation, GPIO, analog front-end, and interrupt
// management. These registers must be configured before any other block.
// ---------------------------------------------------------------------------

// Auto Power State FSM and clock control — the APS FSM transitions the
// chip between power states (card-emu, active, LPS, etc.).  The power-on
// sequence triggers a card-emu → active transition to bring the MAC
// register domain (0x0100+) online.
static const uint16 kRegApsFsmco			= 0x0004;	// APS_FSMCO (16-bit)
static const uint16 kRegSysClkr			= 0x0006;	// SYS_CLKR (16-bit)

// System isolation and function enable
static const uint16 kRegSysCfg				= 0x00F0;
static const uint16 kRegSysFuncEn			= 0x0002;
static const uint16 kRegAfeCtrl1			= 0x0024;
static const uint16 kRegAfeCtrl2			= 0x0028;
static const uint16 kRegAfeCtrl3			= 0x002C;
static const uint16 kRegRsvCtrl			= 0x001C;
static const uint16 kRegApsRsvd			= 0x001E;

// GPIO and pad control
static const uint16 kRegGpioMuxCfg			= 0x0040;
static const uint16 kRegGpioPinCtrl		= 0x0044;
static const uint16 kRegGpioIntm			= 0x0048;
static const uint16 kRegLedCfg				= 0x004C;

// Power sequence control
static const uint16 kRegPwrData			= 0x0038;
static const uint16 kRegCalTimer			= 0x003C;

// SYS_FUNC_EN bit definitions — controls which blocks are powered on
static const uint16 kSysFuncEnBBRSTB		= (1 << 0);
static const uint16 kSysFuncEnBBGlbRst		= (1 << 1);
static const uint16 kSysFuncEnUSBA			= (1 << 2);
static const uint16 kSysFuncEnUPLL			= (1 << 3);
static const uint16 kSysFuncEnUSBD			= (1 << 4);
static const uint16 kSysFuncEnCpuEn		= (1 << 10);  // Lexra 3081 MCU
	// BIT2 of byte 1 at REG_SYS_FUNC_EN+1 — matches _3081Disable8814A()
	// in the reference driver (morrownr/8814au).
static const uint16 kSysFuncEnDcore			= (1 << 13);
static const uint16 kSysFuncEnELDR			= (1 << 14);
static const uint16 kSysFuncEnHWPDN			= (1 << 15);

// Host interrupt mask and status (set 0 and set 1)
static const uint16 kRegHIMR0				= 0x00B0;
static const uint16 kRegHISR0				= 0x00B4;
static const uint16 kRegHIMR1				= 0x00B8;
static const uint16 kRegHISR1				= 0x00BC;

// HIMR0 bit definitions
static const uint32 kHIMR0_RxOK			= (1 << 0);
static const uint32 kHIMR0_RxErr			= (1 << 1);
static const uint32 kHIMR0_TxOK_VO			= (1 << 4);
static const uint32 kHIMR0_TxOK_VI			= (1 << 5);
static const uint32 kHIMR0_TxOK_BE			= (1 << 6);
static const uint32 kHIMR0_TxOK_BK			= (1 << 7);
static const uint32 kHIMR0_TxBcnOK			= (1 << 8);
static const uint32 kHIMR0_TxBcnErr		= (1 << 9);
static const uint32 kHIMR0_C2HCmd			= (1 << 16);
static const uint32 kHIMR0_CPWM			= (1 << 20);

// Multifunction control
static const uint16 kRegMultiFuncCtrl		= 0x0068;


// ---------------------------------------------------------------------------
// Register addresses — Firmware Control (0x0080 area)
//
// The RTL8814AU uses a Lexra 3081 MIPS-derived MCU. Firmware is loaded via
// IDDMA (Internal Data DMA): the host writes firmware data to the TX packet
// buffer, then programs the DDMA engine to transfer it to the MCU's DMEM
// and IRAM memory regions. The MCU is halted during transfer via
// REG_SYS_FUNC_EN bit 12 (kSysFuncEnCpuEn) and resumed after.
//
// Reference: rtl8814a_hal_init.c — FirmwareDownload8814A(),
//            _FWDownloadEnable_8814A(), IDDMADownLoadFW_3081(),
//            _3081Disable8814A(), _3081Enable8814A(), _FWFreeToGo8814A()
//            in morrownr/8814au.
// ---------------------------------------------------------------------------

static const uint16 kRegMcuFwDl				= 0x0080;

// kRegMcuFwDl / REG_8051FW_CTRL_8814A bit definitions (32-bit at 0x0080)
// Byte 0 (0x0080):
static const uint32 kMcuFwDlEn				= (1 << 0);	// FW download enable
static const uint32 kMcuFwDlRdy			= (1 << 1);	// Set by host after write
static const uint32 kMcuFwDlChksumRpt		= (1 << 2);	// Checksum OK from MCU
static const uint32 kMcuMacIniRdy			= (1 << 3);	// MAC init ready
static const uint32 kMcuBBIniRdy			= (1 << 4);	// BB init ready
static const uint32 kMcuRFIniRdy			= (1 << 5);	// RF init ready
static const uint32 kMcuWintiniRdy			= (1 << 6);	// FW init complete (8051)
static const uint32 kMcuRamDlSel			= (1 << 7);	// RAM download select
// Byte 1 (0x0081):
static const uint32 kMcuFwDlChksumEn		= (1 << 12);	// Checksum report enable
static const uint32 kMcuFwDlDisableSim		= (1 << 13);	// Disable simulation mode
// Byte 1 high:
static const uint32 kMcuCpuDlReady			= (1 << 15);	// Lexra 3081 ready — poll this
// Byte 2 (0x0082):
static const uint32 kMcuFwDlPageShift		= 16;			// Page number in [18:16]
static const uint32 kMcuFwDlPageMask		= (0x07 << 16);
static const uint32 kMcuRomDlEn			= (1 << 19);	// ROM download enable

// Firmware beacon-queue download constants.
//
// On the 3081-MCU chip, firmware is NOT written via control transfers to
// the old 8051-style TX buffer window at 0x1000. That path permanently
// locks the 0x1200+ DDMA register space.  Instead, the reference driver
// submits firmware data as a TX packet on the beacon bulk OUT endpoint
// (pipe index kTxQueueBCN = 2, QSEL = kQslBeacon = 0x10), waits for the
// BcnValid acknowledgement bit in REG_FIFOPAGE_CTRL_2+1 (bit 7), then
// triggers an IDDMA from the beacon's location in the TX packet buffer
// to the MCU's DMEM / IRAM regions.
//
// Reference: HalROMDownloadFWRSVDPage8814A(), SetDownLoadFwRsvdPagePkt_8814A(),
//            WaitDownLoadRSVDPageOK_3081() in zebulon2/rtl8814au.
static const uint32 kFwPageSize				= 0x1000;	// 4 KB per IDDMA chunk
static const uint32 kFwHeaderSize			= 64;		// Lexra 3081 firmware header
static const uint32 kFwTxBufPageSize		= 128;		// PAGE_SIZE_8814A
static const uint32 kQslBeacon				= 0x10;		// QSLT_BEACON in TX desc
static const uint32 kFwTxDescOffset			= 40;		// TX desc bytes before payload
	// MEMOffsetInTxPacketBuf = OCPBASE_TXBUF_3081 + (bndy*128) + 40

// TX packet buffer page boundary.  The reference driver derives this as
//   TXPKT_PGNUM_8814A = (2048 - BCNQ_PAGE_NUM_8814 - WOWLAN_PAGE_NUM_8814)
// but for the USB firmware-download path (no WOWLAN reserved pages yet)
// it ends up at the same value used by the beacon-valid test:
//   TXPKT_PGNUM_8814A_WMM = 0x07F5 (2037)
// We use this as the beacon queue boundary during firmware download.
static const uint16 kFwTxPktBufBoundary		= 0x07F5;

// Firmware ready polling — _FWFreeToGo8814A() uses 100 × 50 ms = 5 sec
static const uint32 kFirmwareReadyAttempts	= 100;
static const bigtime_t kFirmwareReadyDelay	= 50000;	// 50 ms between polls

// DDMA polling — IDDMADownLoadFW_3081() uses 20 × 1 ms
static const uint32 kDDMAPollAttempts		= 20;
static const bigtime_t kDDMAPollDelay		= 1000;		// 1 ms between polls

// Beacon valid polling — WaitDownLoadRSVDPageOK_3081() uses 20 × 50 µs
static const uint32 kBcnValidPollAttempts	= 200;
static const bigtime_t kBcnValidPollDelay	= 50;		// 50 µs between polls


// ---------------------------------------------------------------------------
// Register addresses — IDDMA / DDMA (0x1200 area)
//
// The chip's internal Data DMA engine copies data between the TX packet
// buffer and the Lexra 3081 MCU's OCP memory regions (DMEM and IRAM).
// Used during firmware loading to transfer firmware sections from the
// TX buffer (where the host wrote them) to the MCU memory.
//
// Reference: rtl8814a_spec.h — REG_DDMA_CH0SA, REG_DDMA_CH0DA,
//            REG_DDMA_CH0CTRL in morrownr/8814au.
// ---------------------------------------------------------------------------

static const uint16 kRegDDMACh0SA			= 0x1200;	// Source address
static const uint16 kRegDDMACh0DA			= 0x1204;	// Destination address
static const uint16 kRegDDMACh0Ctrl			= 0x1208;	// Control (length + flags)

// DDMA control register bit definitions
static const uint32 kDDMAChOwn				= (1 << 31);	// Start transfer
static const uint32 kDDMAChksumEn			= (1 << 29);	// Enable checksum
static const uint32 kDDMAChksumFail			= (1 << 27);	// Checksum failed
static const uint32 kDDMAChksumRst			= (1 << 25);	// Reset checksum state
static const uint32 kDDMAChksumCont			= (1 << 24);	// Continue checksum accum
static const uint32 kDDMALenMask			= 0x0001FFFF;	// Transfer length [16:0]

// CPU DMEM configuration (DDMA reset control)
static const uint16 kRegCpuDmemCon			= 0x1080;

// Firmware download status bits — written to byte 0 of REG_8051FW_CTRL_8814A
// (0x0080) by the IDDMA transfer handler after each section completes.
// The reference driver writes these in IDDMADownLoadFW_3081() when ls==TRUE.
static const uint8 kImemDlRdy				= (1 << 3);	// IRAM download ready
static const uint8 kImemChksumOk			= (1 << 4);	// IRAM checksum passed
static const uint8 kDmemDlRdy				= (1 << 5);	// DMEM download ready
static const uint8 kDmemChksumOk			= (1 << 6);	// DMEM checksum passed

// OCP base addresses for Lexra 3081 memory regions
static const uint32 kOcpBaseIMem			= 0x00000000;	// Instruction RAM
static const uint32 kOcpBaseDMem			= 0x00200000;	// Data memory
static const uint32 kOcpBaseTxBuf			= 0x18780000;	// TX packet buffer

// Beacon valid bit — byte at kRegFIFOPage + 1, bit 7
static const uint8 kBcnValidBit				= (1 << 7);


// ---------------------------------------------------------------------------
// Register addresses — MAC General Configuration (0x0100 – 0x01FF)
//
// Command register, packet buffer control, DMA configuration, and the
// H2C/C2H mailbox interface for host-firmware communication.
// ---------------------------------------------------------------------------

static const uint16 kRegCR					= 0x0100;

// MSR (Mode Setting Register) — selects the network operating type.
// Without setting this to MSR_INFRA the chip never auto-ACKs frames
// addressed to us, so any AP we associate to retries its assoc-resp /
// data frames, gives up, and drops us from its client table.
static const uint16 kRegMSR					= 0x0102;
static const uint8  kMSR_NoLink				= 0x00;
static const uint8  kMSR_AdHoc				= 0x01;
static const uint8  kMSR_Infra				= 0x02;	// STA in BSS
static const uint8  kMSR_AP					= 0x03;
static const uint16 kRegPBP				= 0x0104;
static const uint16 kRegTrxDmaCfg			= 0x010C;	// REG_TRXDMA_CTRL
static const uint16 kRegTrxFF_BNDY			= 0x0114;

// REG_TRXDMA_CTRL (0x010C) — maps internal hardware queues (VO/VI/BE/BK/
// MG/HI/CM) to USB endpoint priority levels (HIGH/NORMAL/LOW).  Each
// queue has a 2-bit priority field; the three priorities correspond to
// the three enumerated bulk OUT endpoints in 3EP configurations.
//
// Bit layout (from hal_com_reg.h in morrownr/8814au):
//   [4:5]   VOQ    [6:7]   VIQ    [8:9]   BEQ    [10:11] BKQ
//   [12:13] MGQ    [14:15] HIQ    [16:17] CMQ
//
// Priority values (QUEUE_* in the reference):
enum QueuePriority {
	kQueueExtra		= 0,
	kQueueLow		= 1,
	kQueueNormal	= 2,
	kQueueHigh		= 3,
};

static const uint32 kTxDmaVOQShift			= 4;
static const uint32 kTxDmaVIQShift			= 6;
static const uint32 kTxDmaBEQShift			= 8;
static const uint32 kTxDmaBKQShift			= 10;
static const uint32 kTxDmaMGQShift			= 12;
static const uint32 kTxDmaHIQShift			= 14;
static const uint32 kTxDmaCMQShift			= 16;
static const uint32 kTxDmaQMask				= 0x3;
static const uint32 kTxDmaPriorityBit2		= (1 << 2);

// REG_TRXDMA_CTRL bit 2 enables USB RX aggregation — without this, the
// chip never DMAs received frames out over the bulk-IN endpoint.  See
// freebsd_wlan rtl8812a/usb/r12au_init.c::r12au_init_rx_agg() for the
// canonical sequence (this driver is a sibling of 8812A on the 8814A
// silicon, so the same RX-aggregation registers apply).
static const uint16 kTrxDmaCtrlRxDmaAggEn	= 0x0004;

// USB RX DMA aggregation configuration.  Two bytes at REG_RXDMA_AGG_PG_TH:
//   [7:0]  page count threshold (when N pages buffered, flush to USB)
//   [15:8] timeout in ~32us units (flush after timeout regardless of
//          page count, so latency stays bounded for low-rate traffic)
// USB 2.0 defaults from r12au_postattach: dma_size=0x01, dma_time=0x10
// USB 3.0 defaults: dma_size=0x07, dma_time=0x1a
static const uint16 kRxDmaAggUsb2Value = 0x0520;	// time<<8 | size
static const uint16 kRxDmaAggUsb3Value		= 0x1a07;

// REG_RXDMA_PRO controls RX-DMA burst behavior to USB.  Layout:
//   bit 0       (reserved)
//   bit 1       DMA mode
//   bits 2-3    burst count (set to 3)
//   bits 4-5    burst size: 0=USB3, 1=USB2, 2=USB1
//   bits 6-7    (reserved)
// USB 2.0 -> 0x1e (DMA_MODE | (3<<2) | (1<<4))
// USB 3.0 -> 0x0e (DMA_MODE | (3<<2) | (0<<4))
static const uint16 kRegRxDmaPro			= 0x0290;
static const uint8 kRxDmaProBurstSzMask	= 0x30;
static const uint8 kRxDmaProUsb2Value		= 0x1e;
static const uint8 kRxDmaProUsb3Value		= 0x0e;


// CR (command register) bit definitions
static const uint32 kCR_HCI_TxDMA_En		= (1 << 0);
static const uint32 kCR_HCI_RxDMA_En		= (1 << 1);
static const uint32 kCR_TxDMA_En			= (1 << 2);
static const uint32 kCR_RxDMA_En			= (1 << 3);
static const uint32 kCR_Protocol_En			= (1 << 4);
static const uint32 kCR_Schedule_En			= (1 << 5);
static const uint32 kCR_MAC_TX_En			= (1 << 6);
static const uint32 kCR_MAC_RX_En			= (1 << 7);
static const uint32 kCR_Enswbcnio			= (1 << 12);
static const uint32 kCR_EnsecCAMTx			= (1 << 13);
static const uint32 kCR_EnsecCAMRx			= (1 << 14);

// H2C mailboxes — 4 rotating boxes, each with a 4-byte standard and
// 4-byte extended register. The firmware reads these to process commands
// from the host (scan, associate, set channel, power mode, etc.).
static const uint16 kRegHMEBox0				= 0x01D0;
static const uint16 kRegHMEBox1				= 0x01D4;
static const uint16 kRegHMEBox2				= 0x01D8;
static const uint16 kRegHMEBox3				= 0x01DC;
static const uint16 kRegHMEBoxExt0			= 0x01F0;
static const uint16 kRegHMEBoxExt1			= 0x01F4;
static const uint16 kRegHMEBoxExt2			= 0x01F8;
static const uint16 kRegHMEBoxExt3			= 0x01FC;

// Convenience arrays for indexed access in the H2C send loop
static const uint16 kRegHMEBox[kH2CMailboxCount]
	= { 0x01D0, 0x01D4, 0x01D8, 0x01DC };
static const uint16 kRegHMEBoxExt[kH2CMailboxCount]
	= { 0x01F0, 0x01F4, 0x01F8, 0x01FC };


// ---------------------------------------------------------------------------
// Register addresses — TX DMA (0x0200 – 0x027F)
//
// Controls the TX FIFO page allocation and DMA engine. The TX buffer is
// organized as 2048 pages of 128 bytes each (256 KB total), divided among
// 8 hardware queues.
// ---------------------------------------------------------------------------

static const uint16 kRegRQPN				= 0x0200;
static const uint16 kRegFIFOPage			= 0x0204;	// REG_FIFOPAGE_CTRL_2
static const uint16 kRegAutoLLT				= 0x0208;	// REG_AUTO_LLT_8814A
static const uint16 kRegTxDmaOffsetChk		= 0x020C;

// REG_AUTO_LLT_8814A: writing BIT0 triggers hardware Link-List Table
// initialization.  The chip builds the per-queue page linked-list in the
// TX packet buffer; bit 0 auto-clears when init completes.  Must run
// before any bulk OUT transfer — otherwise the MAC cannot move frames
// from the USB FIFO into the beacon/data queues.
static const uint8 kAutoLLTTrigger			= (1 << 0);
static const uint32 kAutoLLTPollAttempts	= 200;
static const bigtime_t kAutoLLTPollDelay	= 50000;	// 50 ms between polls
static const uint16 kRegTxDmaStatus			= 0x0210;
static const uint16 kRegRQPN_NPQ			= 0x0214;

// 8814A-specific page-allocation registers.  Unlike the older 8192-series
// that used REG_RQPN (0x0200), the 8814A uses a 5-register bank at
// 0x0230–0x0240 (one 32-bit register per queue) plus a commit register
// at 0x022C.  Writing 0x80000000 to REG_RQPN_CTRL_2 latches the per-
// queue values into the hardware.  See _InitQueueReservedPage_8814AUsb()
// in morrownr/8814au, hal/rtl8814a/usb/usb_halinit.c.
static const uint16 kRegRQPN_Ctrl_2			= 0x022C;
static const uint16 kRegFIFOPage_Info_1		= 0x0230;	// HPQ page count
static const uint16 kRegFIFOPage_Info_2		= 0x0234;	// LPQ page count
static const uint16 kRegFIFOPage_Info_3		= 0x0238;	// NPQ page count
static const uint16 kRegFIFOPage_Info_4		= 0x023C;	// EPQ page count
static const uint16 kRegFIFOPage_Info_5		= 0x0240;	// PUB page count
static const uint32 kRQPNCommit				= 0x80000000;

// Per-queue page counts matching morrownr/8814au HPQ/LPQ/NPQ/EPQ_PGNUM
// (all 20, decimal) and the computed PUB_PGNUM = 2040 - 4*20 = 1960.
// Total reserves 8 pages for beacon (BCNQ_PAGE_NUM_8814) out of 2048.
static const uint32 kPageNumHPQ				= 20;
static const uint32 kPageNumLPQ				= 20;
static const uint32 kPageNumNPQ				= 20;
static const uint32 kPageNumEPQ				= 20;
static const uint32 kPageNumPUB				= 1960;

// Boundary registers — describe where the beacon queue starts in the
// TX packet buffer.  Matches REG_TXPKTBUF_BCNQ_BDNY_8814A (0x0424),
// REG_MGQ_PGBNDY_8814A (0x047A), and REG_FIFOPAGE_CTRL_2 (0x0204).
static const uint16 kRegTxPktBufBcnQBdy		= 0x0424;
static const uint16 kRegMgQPgBndy			= 0x047A;
// (kRegFIFOPage = 0x0204 is also the boundary register)


// ---------------------------------------------------------------------------
// Register addresses — RX DMA (0x0280 – 0x02FF)
// ---------------------------------------------------------------------------

static const uint16 kRegRxDmaAggPgTh		= 0x0280;
static const uint16 kRegRxPktNum			= 0x0284;
static const uint16 kRegRxDmaCtrl			= 0x0286;
static const uint16 kRegRxDmaStatus			= 0x0288;

// REG_RXFLTMAP0/1/2 (0x06A0/0x06A2/0x06A4) — per-subtype receive filter.
// Each bit gates one of 16 IEEE-802.11 subtypes; if a register is zero,
// the chip drops EVERY frame of that type even when RCR.AMF/ACF/ADF is
// set.  Power-on default is unknown but appears to be zero on this chip
// (no callbacks fire on bulk-IN until these are populated).  See
// freebsd_wlan if_rtwn_rx.c::rtwn_rxfilter_update_mgt() for the canonical
// STA-mode mask.  We use 0xffff (accept everything) during bring-up
// because we want maximum visibility while diagnosing scan results.
static const uint16 kRegRxFltMap0			= 0x06A0;	// management subtypes
static const uint16 kRegRxFltMap1			= 0x06A2;	// control subtypes
static const uint16 kRegRxFltMap2			= 0x06A4;	// data subtypes

// Baseband demodulator enable + TX/RX path masks.  Sibling 8812A driver
// (freebsd_wlan rtl8812a) writes these in r12a_set_band_2ghz to actually
// turn on CCK and OFDM demodulation in the BB.  Without bits 28-29 set
// in OFDMCCK_EN the BB drops every received frame and the MAC RX FIFO
// stays empty (REG_RXPKT_NUM = 0).  See r12a_chan.c for the canonical
// per-band programming.
static const uint16 kRegBBOfdmCckEn			= 0x0808;	// also serves as RX_PATH
static const uint32 kBBOfdmCckEnCck			= 0x10000000;	// CCK demod enable
static const uint32 kBBOfdmCckEnOfdm			= 0x20000000;	// OFDM demod enable
static const uint32 kBBRxPathMaskShift		= 8;		// 4-bit path mask in bits 8-11
static const uint16 kRegBBTxPath			= 0x080C;
static const uint16 kRegBBCckRxPath			= 0x0A04;
static const uint16 kRegBBCckCheck			= 0x0454;
static const uint16 kRegBBRfePinmux0		= 0x0CB0;



// ---------------------------------------------------------------------------
// Register addresses — Protocol Engine (0x0400 – 0x047F)
//
// Rate adaptation, AMPDU aggregation, and MACID station tracking.
// ---------------------------------------------------------------------------

static const uint16 kRegVOParams			= 0x0400;
static const uint16 kRegVIParams			= 0x0404;
static const uint16 kRegBEParams			= 0x0408;
static const uint16 kRegBKParams			= 0x040C;

static const uint16 kRegSpecSIFS			= 0x0428;
static const uint16 kRegMacSpecSIFS			= 0x042C;
static const uint16 kRegSIFS_CTX			= 0x0514;
static const uint16 kRegSIFS_TRX			= 0x0516;

// FWHW TX queue control — byte +2 bit 6 controls "real beacon" processing.
// The reference driver clears this bit during firmware download so the
// beacon packet we submit is kept in TX packet buffer (not air-transmitted).
static const uint16 kRegFwhwTxqCtrl			= 0x0420;

static const uint16 kRegARFR0				= 0x0444;
static const uint16 kRegARFR1				= 0x044C;
static const uint16 kRegARFR2				= 0x0454;
static const uint16 kRegARFR3				= 0x045C;
static const uint16 kRegARFR4				= 0x0464;
static const uint16 kRegARFR5				= 0x046C;

static const uint16 kRegAmpduMaxTime		= 0x0456;
static const uint16 kRegAmpduMaxLength		= 0x0458;

static const uint16 kRegFastEdcaCtrl		= 0x0460;


// ---------------------------------------------------------------------------
// Register addresses — EDCA / Timing (0x0500 – 0x05FF)
//
// WMM queue parameters, beacon timing, and TSF (Timing Synchronization
// Function) management.
// ---------------------------------------------------------------------------

static const uint16 kRegEdcaVoParam			= 0x0500;
static const uint16 kRegEdcaViParam			= 0x0504;
static const uint16 kRegEdcaBeParam			= 0x0508;
static const uint16 kRegEdcaBkParam			= 0x050C;

static const uint16 kRegBcnTcfg			= 0x0510;
static const uint16 kRegPifs				= 0x0512;
static const uint16 kRegAggBreakTime		= 0x051A;
static const uint16 kRegSlot				= 0x051B;
static const uint16 kRegTxPause				= 0x0522;
static const uint16 kRegDIS_TXREQ_CLR		= 0x0523;

static const uint16 kRegBcnInterval			= 0x0554;
static const uint16 kRegAtimWnd				= 0x055A;
static const uint16 kRegBcnDmaTime			= 0x0559;
static const uint16 kRegDrvEarlyInt			= 0x0558;

static const uint16 kRegTsftr				= 0x0560;	// 64-bit TSF timer
static const uint16 kRegTsftrSyncOffset		= 0x0568;
static const uint16 kRegAggBKTime			= 0x0569;

static const uint16 kRegTBTT_Prohibit		= 0x0540;
static const uint16 kRegBcnCtrl				= 0x0550;


// ---------------------------------------------------------------------------
// Register addresses — Wireless MAC (0x0600 – 0x07FF)
//
// MAC address, BSSID, frame filtering (RCR), security CAM (hardware
// encryption), and multi-port BSSID management.
// ---------------------------------------------------------------------------

static const uint16 kRegRCR				= 0x0608;
static const uint16 kRegMAC_ADDR			= 0x0610;	// 6 bytes
static const uint16 kRegBSSID				= 0x0618;	// 6 bytes
static const uint16 kRegMAC_ADDR_1			= 0x0700;	// Port 1 MAC
static const uint16 kRegBSSID_1			= 0x0708;	// Port 1 BSSID

// RCR (Receive Configuration Register) bit definitions — controls which
// frames the hardware passes up to the host vs. filtering silently.
// Per morrownr 8814au hal_com_reg.h.  These are *different* from the
// older r92c bit positions; copying r92c definitions here was a bug
// that left CBSSID filtering effectively off (we were toggling ACF
// and AMF instead).  Without correct CBSSID_DATA, the chip's HW
// decrypt pipeline doesn't match the BSSID and never decrypts.
static const uint32 kRCR_AAP				= (1u << 0);	// Accept all unicast
static const uint32 kRCR_APM				= (1u << 1);	// Accept physical match
static const uint32 kRCR_AM				= (1u << 2);	// Accept multicast
static const uint32 kRCR_AB				= (1u << 3);	// Accept broadcast
static const uint32 kRCR_CBSSID_DATA		= (1u << 6);	// Check BSSID match (data)
static const uint32 kRCR_CBSSID_BCN		= (1u << 7);	// Check BSSID match (beacon)
static const uint32 kRCR_ACRC32			= (1u << 8);	// Accept CRC32 error
static const uint32 kRCR_AICV				= (1u << 9);	// Accept ICV error
static const uint32 kRCR_ADF				= (1u << 11);	// Accept data frames
static const uint32 kRCR_ACF				= (1u << 12);	// Accept ctrl frames
static const uint32 kRCR_AMF				= (1u << 13);	// Accept mgmt frames
static const uint32 kRCR_HTC_LOC_CTRL	= (1u << 14);	// MFC location control
static const uint32 kRCR_APP_BA_SSN		= (1u << 27);	// Append TXBA SSN
static const uint32 kRCR_APP_PHYST_RXFF	= (1u << 28);	// Append PHY status to RXFF
static const uint32 kRCR_APP_ICV			= (1u << 29);	// Retain ICV after decrypt
static const uint32 kRCR_APP_MIC			= (1u << 30);	// Retain MIC after decrypt
static const uint32 kRCR_APPFCS			= (1u << 31);	// Append FCS to RX

// Multicast Address Register — 64-bit hash filter, address 0x0620.
// All-ones = accept all multicast addresses.  Without this, the chip
// drops multicast frames before they reach the HW decrypt stage,
// regardless of CAM/SECCFG state.
static const uint16 kRegMAR					= 0x0620;

// Security configuration
static const uint16 kRegSecCfg				= 0x0680;
static const uint16 kRegCamCmd				= 0x0670;
static const uint16 kRegCamWrite			= 0x0674;
static const uint16 kRegCamRead				= 0x0678;
static const uint16 kRegCamDbg				= 0x067C;

// kRegCamCmd bits.  The CAM is accessed by writing kRegCamWrite with the
// dword to install, then writing kRegCamCmd with POLLING|WRITE|addr,
// then polling kRegCamCmd until POLLING clears (~few µs).  Address is
// (entry_index << 3) + word_index, where word_index 0..7 covers one
// 32-byte CAM entry: word 0 = CTL0, word 1 = CTL1, words 2..5 = key
// bytes, words 6..7 reserved.
static const uint32 kCamCmdPolling		= 0x80000000u;
static const uint32 kCamCmdWrite		= 0x00010000u;
static const uint32 kCamCmdClear		= 0x40000000u;

// Bits for CAM CTL0 (word 0 of each entry).  Layout:
//   [0..1]  KEYID — group-key id (M3's KDE) or 0 for pairwise
//   [2..4]  ALGO — see kCamAlgo* below
//   [15]    VALID — set last to commit the entry
//   [16..23] MAC[0]
//   [24..31] MAC[1]
static const uint32 kCamValid			= 0x00008000u;
static const uint32 kCamGroupKey		= 0x00000040u;	// BIT(6): mark CAM
													// entry as group/multicast key
static const uint32 kCamAlgoNone		= 0x0u;
static const uint32 kCamAlgoWEP40		= 0x1u;
static const uint32 kCamAlgoTKIP		= 0x2u;
static const uint32 kCamAlgoAES			= 0x4u;	// AES-CCMP
static const uint32 kCamAlgoWEP104		= 0x5u;
static const uint32 kCamAlgoShift		= 2;

// Bits for kRegSecCfg.  Low byte (bits 0..7) is the legacy SECCFG
// register; bits 8..15 are the SECCFG2 / extended bits added on
// later chips (8814au included).  Use 16-bit accesses when CHK_KEYID
// is needed.
//
// TXUCKEY_DEF / RXUCKEY_DEF tell the chip to look up the unicast key
// in CAM by peer MAC; TXBCKEY_DEF / RXBCKEY_DEF do the same for
// broadcast (looked up by the keyid in the received CCMP IV header).
// TXENC and RXDEC actually enable hardware crypto on the TX and RX
// paths.  CHK_KEYID is required by morrownr's HW_VAR_SEC_CFG path
// for 8814au-class chips — without it RX broadcast keyid lookup
// fails and the chip silently drops AP-encrypted frames.
static const uint16 kSecCfgTxUcKeyDef	= 0x0001;
static const uint16 kSecCfgRxUcKeyDef	= 0x0002;
static const uint16 kSecCfgTxEnable		= 0x0004;
static const uint16 kSecCfgRxDecEnable	= 0x0008;
static const uint16 kSecCfgNoSKMC		= 0x0020;	// No CAM-by-MAC search for multicast
static const uint16 kSecCfgTxBcKeyDef	= 0x0040;
static const uint16 kSecCfgRxBcKeyDef	= 0x0080;
static const uint16 kSecCfgChkKeyId		= 0x0100;

// Security type encoding in TX/RX descriptors
enum SecurityType {
	kSecurityNone		= 0,
	kSecurityWEP40		= 1,
	kSecurityTKIP		= 2,
	kSecurityAESCCMP	= 3,
	kSecurityWEP104		= 4,
};

// Retry limit
static const uint16 kRegRetryLimit			= 0x042A;
static const uint16 kRegRespSIFSOFDM		= 0x063A;
static const uint16 kRegRespSIFSCCK		= 0x063C;
static const uint16 kRegACKTo				= 0x0640;


// ---------------------------------------------------------------------------
// Register addresses — Baseband / PHY (per-path)
//
// The RTL8814AU has 4 independent RF/BB paths (A, B, C, D), each with its
// own register space at a different base address. All 4 paths must be
// configured independently during initialization and channel switching.
// ---------------------------------------------------------------------------

static const uint16 kBBRegPathA				= 0x2800;
static const uint16 kBBRegPathB				= 0x2C00;
static const uint16 kBBRegPathC				= 0x3800;
static const uint16 kBBRegPathD				= 0x3C00;

static const uint16 kBBRegPathBase[kRfPathCount]
	= { 0x2800, 0x2C00, 0x3800, 0x3C00 };

// Common PHY register offsets (relative to path base)
static const uint16 kRegRFMod				= 0x0000;
static const uint16 kRegAGCRSSITable		= 0x0040;
static const uint16 kRegOFDM0TRxPathEn		= 0x0040;
static const uint16 kRegOFDM0TRMuxPar		= 0x0044;
static const uint16 kRegCCK0AFESetting		= 0x0000;

// RF register access (indirect via BB registers)
static const uint16 kRegRFCtrl				= 0x001C;
static const uint16 kRegRFPara				= 0x0020;
static const uint16 kRegRFReadData			= 0x0024;

// RF transceiver registers — accessed via kRegRFCtrl indirect path
static const uint8 kRfRegMode				= 0x00;
static const uint8 kRfRegChannelStandalone	= 0x18;
static const uint8 kRfRegTxGain				= 0x56;
static const uint8 kRfRegLNA				= 0xDF;


// ---------------------------------------------------------------------------
// TX descriptor format — 40 bytes prepended to every transmitted frame
//
// The driver builds this descriptor before submitting the frame via USB
// bulk OUT. Fields are packed in little-endian format. Bit positions match
// the hardware expectation.
// ---------------------------------------------------------------------------

static const uint32 kTxDescSize = 40;

// TX descriptor DWORD 0 (offset 0x00)
static const uint32 kTxDescPktLen_Shift		= 0;
static const uint32 kTxDescPktLen_Mask		= 0x0000FFFF;
static const uint32 kTxDescOffset_Shift		= 16;
static const uint32 kTxDescOffset_Mask		= 0x00FF0000;
static const uint32 kTxDescBMC				= (1 << 24);	// Broadcast/Multicast
static const uint32 kTxDescHTC				= (1 << 25);	// HT control present
static const uint32 kTxDescLS				= (1 << 26);	// Last segment
static const uint32 kTxDescFS				= (1 << 27);	// First segment
static const uint32 kTxDescOWN				= (1 << 31);	// Owned by hardware

// TX descriptor DWORD 1 (offset 0x04)
static const uint32 kTxDescMACID_Shift		= 0;
static const uint32 kTxDescMACID_Mask		= 0x0000007F;
static const uint32 kTxDescQueueSel_Shift	= 8;
static const uint32 kTxDescQueueSel_Mask	= 0x00001F00;
static const uint32 kTxDescRateID_Shift		= 16;
static const uint32 kTxDescRateID_Mask		= 0x001F0000;
static const uint32 kTxDescSecType_Shift	= 22;
static const uint32 kTxDescSecType_Mask		= 0x00C00000;
static const uint32 kTxDescPktOffset_Shift	= 24;
static const uint32 kTxDescPktOffset_Mask	= 0x1F000000;

// TX descriptor DWORD 2 (offset 0x08)
static const uint32 kTxDescAGGEn			= (1 << 12);
static const uint32 kTxDescBKRdy			= (1 << 13);

// TX descriptor DWORD 3 (offset 0x0C)
static const uint32 kTxDescSeq_Shift		= 16;
static const uint32 kTxDescSeq_Mask			= 0x0FFF0000;

// TX descriptor DWORD 4 (offset 0x10) — rate and bandwidth
static const uint32 kTxDescDataRate_Shift	= 0;
static const uint32 kTxDescDataRate_Mask	= 0x0000007F;
static const uint32 kTxDescDataBW_Shift		= 5;
static const uint32 kTxDescDataBW_Mask		= 0x00000060;	// FIXME: verify
static const uint32 kTxDescRTSEn			= (1 << 12);
static const uint32 kTxDescCTSEn			= (1 << 13);
static const uint32 kTxDescRetryLimit_Shift	= 16;
static const uint32 kTxDescRetryLimitEn		= (1 << 17);
static const uint32 kTxDescDataShort		= (1 << 4);	// Short preamble

// TX descriptor DWORD 5 (offset 0x14)
static const uint32 kTxDescTxPwrOffset_Shift = 0;

// TX descriptor DWORD 9 (offset 0x24)
static const uint32 kTxDescSWDefine_Shift	= 0;


// ---------------------------------------------------------------------------
// RX descriptor format — 24 bytes prepended to every received frame
//
// The hardware writes this descriptor when a frame is received. The driver
// parses it in the RX path to extract frame metadata (length, rate, RSSI).
// ---------------------------------------------------------------------------

static const uint32 kRxDescSize = 24;

// RX descriptor DWORD 0 (offset 0x00)
static const uint32 kRxDescPktLen_Shift		= 0;
static const uint32 kRxDescPktLen_Mask		= 0x00003FFF;
static const uint32 kRxDescCRC32_Err		= (1 << 14);
static const uint32 kRxDescICV_Err			= (1 << 15);
static const uint32 kRxDescDrvInfoSize_Shift = 16;
static const uint32 kRxDescDrvInfoSize_Mask	= 0x000F0000;
static const uint32 kRxDescShift_Shift		= 24;
static const uint32 kRxDescShift_Mask		= 0x03000000;
static const uint32 kRxDescPHYStatus		= (1 << 26);
static const uint32 kRxDescSWDec			= (1 << 27);
static const uint32 kRxDescLS				= (1 << 28);
static const uint32 kRxDescFS				= (1 << 29);
static const uint32 kRxDescEOR				= (1 << 30);
static const uint32 kRxDescOWN				= (1 << 31);

// RX descriptor DWORD 1 (offset 0x04)
static const uint32 kRxDescMACID_Shift		= 0;
static const uint32 kRxDescMACID_Mask		= 0x0000007F;
static const uint32 kRxDescTID_Shift		= 8;
static const uint32 kRxDescTID_Mask			= 0x00000F00;
static const uint32 kRxDescSecType_Shift	= 20;
static const uint32 kRxDescSecType_Mask		= 0x00700000;

// RX descriptor DWORD 2 (offset 0x08)
static const uint32 kRxDescSeq_Shift		= 0;
static const uint32 kRxDescSeq_Mask			= 0x00000FFF;
static const uint32 kRxDescFrag_Shift		= 12;
static const uint32 kRxDescFrag_Mask		= 0x0000F000;

// RX descriptor DWORD 3 (offset 0x0C) — rate and bandwidth
static const uint32 kRxDescRxRate_Shift		= 0;
static const uint32 kRxDescRxRate_Mask		= 0x0000007F;
static const uint32 kRxDescBW_Shift			= 4;
static const uint32 kRxDescBW_Mask			= 0x00000030;


// ---------------------------------------------------------------------------
// Data rate indices
//
// Used in both TX and RX descriptors. CCK rates for 2.4 GHz, OFDM for both
// bands, HT (802.11n) MCS indices, VHT (802.11ac) MCS indices.
// ---------------------------------------------------------------------------

enum DataRateIndex {
	// CCK rates (2.4 GHz only)
	kRateCCK1		= 0x00,
	kRateCCK2		= 0x01,
	kRateCCK5_5		= 0x02,
	kRateCCK11		= 0x03,

	// OFDM rates
	kRateOFDM6		= 0x04,
	kRateOFDM9		= 0x05,
	kRateOFDM12		= 0x06,
	kRateOFDM18		= 0x07,
	kRateOFDM24		= 0x08,
	kRateOFDM36		= 0x09,
	kRateOFDM48		= 0x0A,
	kRateOFDM54		= 0x0B,

	// HT MCS indices (802.11n)
	kRateHT_MCS0	= 0x0C,
	kRateHT_MCS1	= 0x0D,
	kRateHT_MCS2	= 0x0E,
	kRateHT_MCS3	= 0x0F,
	kRateHT_MCS4	= 0x10,
	kRateHT_MCS5	= 0x11,
	kRateHT_MCS6	= 0x12,
	kRateHT_MCS7	= 0x13,
	kRateHT_MCS8	= 0x14,
	kRateHT_MCS9	= 0x15,
	kRateHT_MCS10	= 0x16,
	kRateHT_MCS11	= 0x17,
	kRateHT_MCS12	= 0x18,
	kRateHT_MCS13	= 0x19,
	kRateHT_MCS14	= 0x1A,
	kRateHT_MCS15	= 0x1B,

	// VHT MCS indices (802.11ac) — per spatial stream
	kRateVHT_1SS_MCS0	= 0x2C,
	kRateVHT_1SS_MCS1	= 0x2D,
	kRateVHT_1SS_MCS2	= 0x2E,
	kRateVHT_1SS_MCS3	= 0x2F,
	kRateVHT_1SS_MCS4	= 0x30,
	kRateVHT_1SS_MCS5	= 0x31,
	kRateVHT_1SS_MCS6	= 0x32,
	kRateVHT_1SS_MCS7	= 0x33,
	kRateVHT_1SS_MCS8	= 0x34,
	kRateVHT_1SS_MCS9	= 0x35,

	kRateVHT_2SS_MCS0	= 0x36,
	kRateVHT_2SS_MCS9	= 0x3F,

	kRateVHT_3SS_MCS0	= 0x40,
	kRateVHT_3SS_MCS9	= 0x49,
};


// ---------------------------------------------------------------------------
// Channel and bandwidth definitions
// ---------------------------------------------------------------------------

enum ChannelBandwidth {
	kBandwidth20MHz		= 0,
	kBandwidth40MHz		= 1,
	kBandwidth80MHz		= 2,
};

enum ChannelBand {
	kBand2_4GHz			= 0,
	kBand5GHz			= 1,
};

// 2.4 GHz channels (1–14)
static const uint8 kChannelList2G[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};

// 5 GHz channels — UNII-1, UNII-2, UNII-2 Extended, UNII-3
static const uint8 kChannelList5G[] = {
	36, 40, 44, 48,							// UNII-1
	52, 56, 60, 64,							// UNII-2
	100, 104, 108, 112, 116, 120, 124, 128,	// UNII-2 Extended
	132, 136, 140, 144,						// UNII-2 Extended (cont.)
	149, 153, 157, 161, 165, 169, 173, 177	// UNII-3
};


// ---------------------------------------------------------------------------
// H2C command IDs — sent from host to firmware via the mailbox system
//
// The firmware running on the Lexra 3081 processes these commands to perform
// operations the driver cannot do directly (scan, authenticate, manage
// power states, etc.).
// ---------------------------------------------------------------------------

enum H2CCommandID {
	kH2C_RsvdPage			= 0x00,
	kH2C_MediaStatusRpt	= 0x01,
	kH2C_ScanEn			= 0x02,
	kH2C_KeepAlive			= 0x03,
	kH2C_SetPwrMode		= 0x05,
	kH2C_PSTunePar			= 0x06,
	kH2C_MacIDCfg			= 0x40,
	kH2C_RSSI_Setting		= 0x42,
	kH2C_APReqTxRpt			= 0x43,
	kH2C_RaInfo				= 0x44,
	kH2C_BcnRsvdPage		= 0x09,
	kH2C_WoWLAN				= 0x80,
	kH2C_RemoteWakeCtrl		= 0x81,
};


// ---------------------------------------------------------------------------
// C2H event IDs — sent from firmware to host via interrupt IN endpoint
// ---------------------------------------------------------------------------

enum C2HEventID {
	kC2H_Debug				= 0x01,
	kC2H_ScanComplete		= 0x07,
	kC2H_BtInfo			= 0x09,
	kC2H_RateAdaptive		= 0x0C,
	kC2H_ConnectionStatus	= 0x10,
	kC2H_TxReport			= 0x14,
};


// ---------------------------------------------------------------------------
// EFUSE map field offsets — factory-programmed calibration data
//
// The EFUSE stores per-device calibration including MAC address, TX power
// tables, antenna configuration, and regulatory domain. These offsets are
// for the logical EFUSE map (after decoding the physical EFUSE).
// ---------------------------------------------------------------------------

static const uint16 kEfuseMacAddr			= 0x0D8;	// 6 bytes (USB variant)
static const uint16 kEfuseAntennaConfig		= 0x00E;	// TX + RX path config
static const uint16 kEfuseRfeType			= 0x010;	// RF front-end type (0-6)
static const uint16 kEfuseTxPwr2G			= 0x020;	// 2.4 GHz power table
static const uint16 kEfuseTxPwr5G			= 0x060;	// 5 GHz power table
static const uint16 kEfuseTxPwrByRate		= 0x0B0;	// Power-by-rate diffs
static const uint16 kEfuseThermalMeter		= 0x100;	// Thermal calibration
static const uint16 kEfuseCrystalCal		= 0x120;	// Crystal calibration
static const uint16 kEfuseChannelPlan		= 0x130;	// Regulatory domain


// ---------------------------------------------------------------------------
// Power-on sequence step types — used by the hardware init state machine
//
// The power-on sequence is a series of register writes, delays, and polls
// that bring the chip from reset to an operational state. Derived from
// Hal8814PwrSeq.c in the reference driver.
// ---------------------------------------------------------------------------

enum PowerSeqCmdType {
	kPwrCmdWrite		= 0,	// Write value to register
	kPwrCmdPolling		= 1,	// Poll register until condition met
	kPwrCmdDelay		= 2,	// Delay in microseconds
	kPwrCmdEnd			= 3,	// End of sequence
};

struct PowerSeqCommand {
	uint16	offset;				// Register address
	uint8	cutMask;			// Chip revision mask (0xFF = all)
	uint8	cmdType;			// PowerSeqCmdType
	uint8	mask;				// Bit mask
	uint8	value;				// Value to write or expect
};


#endif	// RTL8814AU_H

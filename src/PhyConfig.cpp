/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * PhyConfig.cpp — PHY/RF/BB configuration for RTL8814AU.
 *
 * Initializes and configures the 4-path radio system:
 *   1. Load baseband register tables (compiled-in, chip-specific)
 *   2. Configure each RF path's transceiver registers
 *   3. Run IQ calibration to compensate signal imbalance
 *   4. Set TX power from EFUSE calibration tables
 *   5. Channel switching with band-aware reconfiguration
 *
 * The BB and RF register tables are large (~1200 BB entries, ~1400-1600
 * RF entries per path) and are stored as compiled-in PhyRegEntry arrays.
 * These tables are defined in PhyRegTables.h and derived from the
 * reference driver's halhwimg8814a_bb.c and halhwimg8814a_rf.c.
 *
 * The reference driver uses a conditional table format with special flag
 * values (0x80000000-0xBFFFFFFF range) to select entries based on chip
 * revision. Our tables use the default/else values that apply to all
 * revisions — this is safe for initial operation and covers the most
 * common hardware variants.
 *
 * Reference: rtl8814a_phycfg.c, hal/phydm/rtl8814a/halhwimg8814a_bb.c,
 * halhwimg8814a_rf.c in ulli-kroll/rtl8814au.
 */

#include "PhyConfig.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include "EfuseReader.h"
#include "PhyRegTables.h"
#include "RegisterIO.h"


// ---------------------------------------------------------------------------
// BB bandwidth configuration registers
//
// These registers control the baseband filter bandwidth. The values are
// derived from rtl8814a_phycfg.c:PHY_SetBWMode8814A().
// ---------------------------------------------------------------------------

// Register 0x8AC: primary channel and bandwidth mode
static const uint16 kRegBBBwCtrl = 0x08AC;

// Register 0x668: sub-channel position for 40/80 MHz
static const uint16 kRegBBSubChan = 0x0668;

// Register 0x8C4: ADC clock and filter bandwidth
static const uint16 kRegBBAdcClk = 0x08C4;

// Register 0x8C8: DAC clock mode
static const uint16 kRegBBDacClk = 0x08C8;


// ---------------------------------------------------------------------------
// TX power registers per path
//
// Each RF path has dedicated TX power index registers for different rate
// groups. The firmware uses these to set the actual transmit power.
// ---------------------------------------------------------------------------

// TX power index registers — CCK rates (2.4 GHz only)
static const uint16 kRegTxPwrCCK[kRfPathCount]
	= { 0x0C20, 0x0E20, 0x1820, 0x1A20 };

// TX power index registers — OFDM rates
static const uint16 kRegTxPwrOFDM[kRfPathCount]
	= { 0x0C24, 0x0E24, 0x1824, 0x1A24 };

// TX power index registers — HT MCS 0-7
static const uint16 kRegTxPwrHT1[kRfPathCount]
	= { 0x0C28, 0x0E28, 0x1828, 0x1A28 };

// TX power index registers — HT MCS 8-15
static const uint16 kRegTxPwrHT2[kRfPathCount]
	= { 0x0C2C, 0x0E2C, 0x182C, 0x1A2C };

// TX power index registers — VHT 1SS MCS 0-9
static const uint16 kRegTxPwrVHT1[kRfPathCount]
	= { 0x0C30, 0x0E30, 0x1830, 0x1A30 };

// TX power index registers — VHT 2SS MCS 0-9
static const uint16 kRegTxPwrVHT2[kRfPathCount]
	= { 0x0C34, 0x0E34, 0x1834, 0x1A34 };


// ---------------------------------------------------------------------------
// IQ calibration registers
// ---------------------------------------------------------------------------

// IQ calibration trigger register
static const uint16 kRegIQKCtrl = 0x0E28;

// Per-path IQ calibration result registers (TX I/Q coefficients)
static const uint16 kRegIQKTxI[kRfPathCount]
	= { 0x0C80, 0x0E80, 0x1880, 0x1A80 };
static const uint16 kRegIQKTxQ[kRfPathCount]
	= { 0x0C94, 0x0E94, 0x1894, 0x1A94 };

// Per-path IQ calibration result registers (RX I/Q coefficients)
static const uint16 kRegIQKRxI[kRfPathCount]
	= { 0x0C10, 0x0E10, 0x1810, 0x1A10 };
static const uint16 kRegIQKRxQ[kRfPathCount]
	= { 0x0C14, 0x0E14, 0x1814, 0x1A14 };


// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


RTL8814AUPhyConfig::RTL8814AUPhyConfig(RTL8814AURegisterIO* registerIO,
	RTL8814AUEfuseReader* efuseReader)
	:
	fRegisterIO(registerIO),
	fEfuseReader(efuseReader),
	fCurrentChannel(1),
	fCurrentBandwidth(kBandwidth20MHz),
	fCurrentBand(kBand2_4GHz),
	fInitialized(false)
{
	memset(fTxPowerIndex, 0, sizeof(fTxPowerIndex));
}


RTL8814AUPhyConfig::~RTL8814AUPhyConfig()
{
}


// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------


/*! Full PHY initialization. Must be called after firmware is loaded and
    EFUSE has been read (the TX power tables come from EFUSE).
*/
status_t
RTL8814AUPhyConfig::Initialize()
{
	dprintf(RTL8814AU_DRIVER_NAME ": replaying morrownr cold-start init "
		"sequence (%" B_PRIu32 " writes)\n", kFullInitSequenceCount);

	status_t status = _ApplyBBTable(kFullInitSequence,
		kFullInitSequenceCount);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": full init replay failed: %s\n",
			strerror(status));
		return status;
	}

	fInitialized = true;
	dprintf(RTL8814AU_DRIVER_NAME ": PHY initialization complete\n");
	return B_OK;

}


/*! Switch the radio to a new channel and bandwidth.

    \param channel    Channel number (1-14 for 2.4 GHz, 36-177 for 5 GHz)
    \param bandwidth  Channel width (20/40/80 MHz)
    \return B_OK on success.
*/
status_t
RTL8814AUPhyConfig::SetChannel(uint8 channel, ChannelBandwidth bandwidth)
{
	ChannelBand newBand = _BandForChannel(channel);
	bool bandChanged = (newBand != fCurrentBand);

	dprintf(RTL8814AU_DRIVER_NAME ": setting channel %u, BW %u, "
		"band %s%s\n", channel,
		bandwidth == kBandwidth20MHz ? 20
			: bandwidth == kBandwidth40MHz ? 40 : 80,
		newBand == kBand2_4GHz ? "2.4 GHz" : "5 GHz",
		bandChanged ? " (band switch)" : "");

	// Configure the RF synthesizer on each path for the new frequency.
	// The channel number maps directly to a synthesizer setting via
	// RF register 0x18.
	for (uint32 path = 0; path < kRfPathCount; path++) {
		status_t status = _WriteRF(path, kRfRegChannelStandalone, channel);
		if (status != B_OK)
			return status;
	}

	// If the band changed (2.4 ↔ 5 GHz), reconfigure the baseband
	if (bandChanged) {
		// Switch LNA mode for the new band
		uint32 lnaValue = (newBand == kBand5GHz) ? 0x00001 : 0x00000;
		for (uint32 path = 0; path < kRfPathCount; path++)
			_WriteRF(path, kRfRegLNA, lnaValue);

		// Reload band-specific AGC tables — the 2.4 GHz and 5 GHz bands
		// have different AGC gain curves due to different RF front-end
		// gain/noise characteristics
		if (newBand == kBand5GHz) {
			dprintf(RTL8814AU_DRIVER_NAME ": reloading AGC tables for "
				"5 GHz\n");
			_ApplyBBTable(kAGCTable5G, kAGCTable5GCount);
		} else {
			dprintf(RTL8814AU_DRIVER_NAME ": reloading AGC tables for "
				"2.4 GHz\n");
			_ApplyBBTable(kAGCTable2G, kAGCTable2GCount);
		}
	}

	// Configure bandwidth in the baseband registers
	status_t status = _SetBandwidth(bandwidth);
	if (status != B_OK)
		return status;

	// Update TX power for the new channel — look up per-channel power
	// index from the EFUSE calibration tables
	for (uint32 path = 0; path < kRfPathCount; path++) {
		uint8 powerIndex = _GetTxPowerIndex(path, channel);
		_WriteTxPowerIndex(path, powerIndex);
	}

	fCurrentChannel = channel;
	fCurrentBandwidth = bandwidth;
	fCurrentBand = newBand;

	return B_OK;
}


// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------


/*! Load the baseband register initialization table. Programs the core
    BB registers that control modulation, timing, path enables, and AGC.

    The BB table covers: common BB config (0x800–0x8FF), OFDM/CCK
    receiver settings, and per-path digital TX/RX control (paths A–D).
    The AGC gain curve tables are applied separately, band-specific.

    Tables are defined in PhyRegTables.h and derived from the reference
    driver's Array_MP_8814A_PHY_REG and Array_MP_8814A_AGC_TAB.
*/
status_t
RTL8814AUPhyConfig::_InitBBRegisters()
{
	dprintf(RTL8814AU_DRIVER_NAME ": loading BB register tables "
		"(%" B_PRIu32 " entries)\n", kBBInitTableCount);

	// Apply the main BB register table (common + per-path digital config)
	status_t status = _ApplyBBTable(kBBInitTable, kBBInitTableCount);
	if (status != B_OK)
		return status;

	// Apply the initial AGC table — start with 2.4 GHz since we default
	// to channel 1
	dprintf(RTL8814AU_DRIVER_NAME ": loading AGC tables for 2.4 GHz "
		"(%" B_PRIu32 " entries)\n", kAGCTable2GCount);

	status = _ApplyBBTable(kAGCTable2G, kAGCTable2GCount);
	if (status != B_OK)
		return status;

	dprintf(RTL8814AU_DRIVER_NAME ": BB init complete\n");
	return B_OK;
}


/*! Configure the RF transceiver registers on each of the 4 paths.
    Each path has its own RF register space accessed indirectly through
    the baseband registers.

    For each path we apply:
      1. The common RF init sequence (PLL, synthesizer, AGC, PA/LNA)
      2. Path-specific trim values (TX/RX DC offsets, IQ trim, PA bias)

    Tables are defined in PhyRegTables.h.
*/
status_t
RTL8814AUPhyConfig::_InitRFRegisters()
{
	// No-op — RF programming is now part of kFullInitSequence
	// (replayed by Initialize()).
	return B_OK;

}


/*! Run IQ calibration on all 4 paths. IQ calibration corrects for
    amplitude and phase imbalance between the I and Q signal components.

    The procedure for the RTL8814AU:
      1. Save current BB/RF register state
      2. Configure paths into calibration mode
      3. Send calibration tones on each path
      4. Measure I/Q mismatch via calibration result registers
      5. Compute correction coefficients
      6. Write coefficients to the IQ compensation registers
      7. Restore register state

    Reference: halphyrf_8814a_ce.c:PHY_IQCalibrate_8814A()
*/
status_t
RTL8814AUPhyConfig::_RunIQCalibration()
{
	dprintf(RTL8814AU_DRIVER_NAME ": running IQ calibration\n");

	// Save registers that will be modified during calibration
	uint32 savedBBRegs[4];
	savedBBRegs[0] = fRegisterIO->Read32(0x0C60);
	savedBBRegs[1] = fRegisterIO->Read32(0x0E60);
	savedBBRegs[2] = fRegisterIO->Read32(0x1860);
	savedBBRegs[3] = fRegisterIO->Read32(0x1A60);

	// Enable IQ calibration mode
	fRegisterIO->Write32(kRegIQKCtrl, 0x00000000);

	for (uint32 path = 0; path < kRfPathCount; path++) {
		// Configure the calibration engine for this path
		// Set TX IQ calibration mode
		fRegisterIO->Write32(kRegIQKTxI[path], 0x00000000);
		fRegisterIO->Write32(kRegIQKTxQ[path], 0x00000000);

		// Trigger calibration on this path by writing to the path's
		// IQ calibration trigger register
		uint16 base = kBBRegPathBase[path];
		fRegisterIO->Write32(base + 0x0060, 0x80015C00);

		// Wait for calibration to complete (~10 ms per path)
		snooze(10000);

		// Read calibration results
		uint32 txI = fRegisterIO->Read32(kRegIQKTxI[path]);
		uint32 txQ = fRegisterIO->Read32(kRegIQKTxQ[path]);

		// Verify the calibration produced valid results.
		// A failed calibration returns all zeros or all ones.
		bool valid = (txI != 0 && txI != 0xFFFFFFFF
			&& txQ != 0 && txQ != 0xFFFFFFFF);

		if (valid) {
			// Write the compensation coefficients to the correction
			// registers. The hardware applies these in real-time.
			fRegisterIO->Write32(kRegIQKRxI[path], txI);
			fRegisterIO->Write32(kRegIQKRxQ[path], txQ);
			dprintf(RTL8814AU_DRIVER_NAME ": IQ cal path %c: "
				"I=0x%08" B_PRIx32 " Q=0x%08" B_PRIx32 "\n",
				'A' + path, txI, txQ);
		} else {
			dprintf(RTL8814AU_DRIVER_NAME ": IQ cal path %c: "
				"invalid results, using defaults\n", 'A' + path);
		}
	}

	// Restore saved registers
	fRegisterIO->Write32(0x0C60, savedBBRegs[0]);
	fRegisterIO->Write32(0x0E60, savedBBRegs[1]);
	fRegisterIO->Write32(0x1860, savedBBRegs[2]);
	fRegisterIO->Write32(0x1A60, savedBBRegs[3]);

	// Disable IQ calibration mode
	fRegisterIO->Write32(kRegIQKCtrl, 0x00000000);

	return B_OK;
}


/*! Set TX power on all paths based on EFUSE calibration data.
    Reads per-path, per-channel-group power indices from the EFUSE map
    and programs them into the TX power registers.
*/
status_t
RTL8814AUPhyConfig::_SetTxPower()
{
	dprintf(RTL8814AU_DRIVER_NAME ": configuring TX power from EFUSE\n");

	const uint8* efuseMap = fEfuseReader->Map();
	if (efuseMap == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map not available, "
			"using default TX power\n");
		// Use a safe default power index (middle of range)
		for (uint32 path = 0; path < kRfPathCount; path++) {
			for (uint32 group = 0; group < kTxPwrGroupCountTotal; group++)
				fTxPowerIndex[path][group] = 0x24;	// ~20 dBm
		}
		goto apply;
	}

	// Read 2.4 GHz TX power indices from EFUSE.
	// The EFUSE stores per-path power for 5 channel groups at kEfuseTxPwr2G.
	// Layout: path A groups 0-4, then path B groups 0-4, etc.
	for (uint32 path = 0; path < kRfPathCount; path++) {
		uint16 offset2G = kEfuseTxPwr2G + path * kTxPwrGroupCount2G;
		for (uint32 group = 0; group < kTxPwrGroupCount2G; group++) {
			if (offset2G + group < kEfuseMapSize)
				fTxPowerIndex[path][group] = efuseMap[offset2G + group];
			else
				fTxPowerIndex[path][group] = 0x24;
		}

		// Read 5 GHz TX power indices
		uint16 offset5G = kEfuseTxPwr5G + path * kTxPwrGroupCount5G;
		for (uint32 group = 0; group < kTxPwrGroupCount5G; group++) {
			uint32 idx = kTxPwrGroupCount2G + group;
			if (offset5G + group < kEfuseMapSize)
				fTxPowerIndex[path][idx] = efuseMap[offset5G + group];
			else
				fTxPowerIndex[path][idx] = 0x24;
		}
	}

apply:
	// Apply the power indices for the current channel
	for (uint32 path = 0; path < kRfPathCount; path++) {
		uint8 powerIndex = _GetTxPowerIndex(path, fCurrentChannel);
		status_t status = _WriteTxPowerIndex(path, powerIndex);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


// ---------------------------------------------------------------------------
// Table processing
// ---------------------------------------------------------------------------


/*! Apply a BB register initialization table by writing each entry to
    the baseband registers via the register I/O module.
*/
status_t
RTL8814AUPhyConfig::_ApplyBBTable(const PhyRegEntry* table, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		status_t status = fRegisterIO->Write32(
			(uint16)table[i].address, table[i].value);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": BB table write failed at "
				"entry %" B_PRIu32 " (0x%04" B_PRIx32 ")\n",
				i, table[i].address);
			return status;
		}

		// Brief delay between writes — some BB registers need settling
		// time, especially PLL and clock-related registers
		if ((table[i].address & 0xFF00) == 0x0800)
			snooze(1);
	}
	return B_OK;
}


/*! Apply an RF register initialization table to a specific path.
    RF registers are written via the indirect BB interface.
*/
status_t
RTL8814AUPhyConfig::_ApplyRFTable(uint32 path, const PhyRegEntry* table,
	uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		status_t status = _WriteRF(path, (uint8)table[i].address,
			table[i].value);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": RF table write failed "
				"path %c entry %" B_PRIu32 " (reg 0x%02" B_PRIx32 ")\n",
				'A' + path, i, table[i].address);
			return status;
		}
	}
	return B_OK;
}


// ---------------------------------------------------------------------------
// TX power helpers
// ---------------------------------------------------------------------------


/*! Look up the TX power index for a given path and channel by mapping
    the channel to a power group and reading from the cached EFUSE data.
*/
uint8
RTL8814AUPhyConfig::_GetTxPowerIndex(uint32 path, uint8 channel)
{
	if (path >= kRfPathCount)
		return 0x24;

	uint32 groupIndex = _ChannelToGroupIndex(channel);
	if (groupIndex >= kTxPwrGroupCountTotal)
		return 0x24;

	return fTxPowerIndex[path][groupIndex];
}


/*! Write the TX power index to all rate-group registers for a path.
    The same base power index is used for all rates; the hardware applies
    per-rate offsets internally from the power-by-rate table.
*/
status_t
RTL8814AUPhyConfig::_WriteTxPowerIndex(uint32 path, uint8 powerIndex)
{
	if (path >= kRfPathCount)
		return B_BAD_INDEX;

	// Build a 4-byte value with the same index in each byte position
	// (one byte per rate in the group)
	uint32 powerVal = (uint32)powerIndex
		| ((uint32)powerIndex << 8)
		| ((uint32)powerIndex << 16)
		| ((uint32)powerIndex << 24);

	fRegisterIO->Write32(kRegTxPwrCCK[path], powerVal);
	fRegisterIO->Write32(kRegTxPwrOFDM[path], powerVal);
	fRegisterIO->Write32(kRegTxPwrHT1[path], powerVal);
	fRegisterIO->Write32(kRegTxPwrHT2[path], powerVal);
	fRegisterIO->Write32(kRegTxPwrVHT1[path], powerVal);
	fRegisterIO->Write32(kRegTxPwrVHT2[path], powerVal);

	return B_OK;
}


/*! Map a channel number to a TX power group index.

    2.4 GHz channel groups (5 groups):
      Group 0: channels 1-2
      Group 1: channels 3-5
      Group 2: channels 6-8
      Group 3: channels 9-11
      Group 4: channels 12-14

    5 GHz channel groups (9 groups):
      Group 5: channels 36-48   (UNII-1)
      Group 6: channels 52-64   (UNII-2)
      Group 7: channels 100-116 (UNII-2 Ext lower)
      Group 8: channels 120-128 (UNII-2 Ext mid)
      Group 9: channels 132-144 (UNII-2 Ext upper)
      Group 10: channels 149-153 (UNII-3 lower)
      Group 11: channels 157-161 (UNII-3 mid)
      Group 12: channels 165-169 (UNII-3 upper)
      Group 13: channels 173-177 (UNII-3 top)
*/
uint32
RTL8814AUPhyConfig::_ChannelToGroupIndex(uint8 channel)
{
	if (channel <= 2)	return 0;
	if (channel <= 5)	return 1;
	if (channel <= 8)	return 2;
	if (channel <= 11)	return 3;
	if (channel <= 14)	return 4;

	// 5 GHz
	if (channel <= 48)	return 5;
	if (channel <= 64)	return 6;
	if (channel <= 116)	return 7;
	if (channel <= 128)	return 8;
	if (channel <= 144)	return 9;
	if (channel <= 153)	return 10;
	if (channel <= 161)	return 11;
	if (channel <= 169)	return 12;
	return 13;
}


// ---------------------------------------------------------------------------
// Bandwidth configuration
// ---------------------------------------------------------------------------


/*! Configure the baseband for the specified channel bandwidth.
    This adjusts the ADC/DAC filter width and sub-channel position.

    Reference: PHY_SetBWMode8814A() in rtl8814a_phycfg.c
*/
status_t
RTL8814AUPhyConfig::_SetBandwidth(ChannelBandwidth bandwidth)
{
	switch (bandwidth) {
		case kBandwidth20MHz:
		{
			// 20 MHz: narrowest filter, no sub-channel offset
			fRegisterIO->MaskedWrite32(kRegBBBwCtrl, 0x00000003, 0x00000000);
			fRegisterIO->MaskedWrite32(kRegBBSubChan, 0x0000000F, 0x00000000);
			fRegisterIO->MaskedWrite32(kRegBBAdcClk, 0x30000000, 0x00000000);
			fRegisterIO->MaskedWrite32(kRegBBDacClk, 0x30000000, 0x00000000);

			// Set RF filter bandwidth to 20 MHz on all paths
			for (uint32 path = 0; path < kRfPathCount; path++) {
				uint32 rfValue = _ReadRF(path, 0x18);
				rfValue &= ~(0x03 << 10);	// Clear BW bits [11:10]
				_WriteRF(path, 0x18, rfValue);
			}
			break;
		}

		case kBandwidth40MHz:
		{
			// 40 MHz: medium filter
			fRegisterIO->MaskedWrite32(kRegBBBwCtrl, 0x00000003, 0x00000001);
			fRegisterIO->MaskedWrite32(kRegBBSubChan, 0x0000000F, 0x00000000);
			fRegisterIO->MaskedWrite32(kRegBBAdcClk, 0x30000000, 0x10000000);
			fRegisterIO->MaskedWrite32(kRegBBDacClk, 0x30000000, 0x10000000);

			for (uint32 path = 0; path < kRfPathCount; path++) {
				uint32 rfValue = _ReadRF(path, 0x18);
				rfValue &= ~(0x03 << 10);
				rfValue |= (0x01 << 10);	// 40 MHz
				_WriteRF(path, 0x18, rfValue);
			}
			break;
		}

		case kBandwidth80MHz:
		{
			// 80 MHz: widest filter (802.11ac)
			fRegisterIO->MaskedWrite32(kRegBBBwCtrl, 0x00000003, 0x00000002);
			fRegisterIO->MaskedWrite32(kRegBBSubChan, 0x0000000F, 0x00000000);
			fRegisterIO->MaskedWrite32(kRegBBAdcClk, 0x30000000, 0x20000000);
			fRegisterIO->MaskedWrite32(kRegBBDacClk, 0x30000000, 0x20000000);

			for (uint32 path = 0; path < kRfPathCount; path++) {
				uint32 rfValue = _ReadRF(path, 0x18);
				rfValue &= ~(0x03 << 10);
				rfValue |= (0x02 << 10);	// 80 MHz
				_WriteRF(path, 0x18, rfValue);
			}
			break;
		}
	}

	dprintf(RTL8814AU_DRIVER_NAME ": bandwidth set to %u MHz\n",
		bandwidth == kBandwidth20MHz ? 20
			: bandwidth == kBandwidth40MHz ? 40 : 80);

	return B_OK;
}


// ---------------------------------------------------------------------------
// RF register access
// ---------------------------------------------------------------------------


/*! Write a value to an RF register on a specific path. RF registers are
    accessed indirectly: write the address and data to the BB's RF control
    register for the given path, then wait for the write to complete.

    \param path        RF path index (0=A, 1=B, 2=C, 3=D)
    \param rfRegister  RF register address (0x00-0xFF)
    \param value       Value to write (20-bit RF data)
    \return B_OK on success.
*/
status_t
RTL8814AUPhyConfig::_WriteRF(uint32 path, uint8 rfRegister, uint32 value)
{
	if (path >= kRfPathCount)
		return B_BAD_INDEX;

	// The RF write interface uses the BB register at (path_base + kRegRFCtrl).
	// Format: [19:0] = data, [27:20] = RF register address
	uint16 base = kBBRegPathBase[path];
	uint32 rfValue = (value & 0x000FFFFF) | ((uint32)rfRegister << 20);

	return fRegisterIO->Write32(base + kRegRFCtrl, rfValue);
}


/*! Read an RF register on a specific path via indirect access.

    \param path        RF path index (0=A, 1=B, 2=C, 3=D)
    \param rfRegister  RF register address
    \return Register value (20-bit), or 0xFFFFFFFF on error.
*/
uint32
RTL8814AUPhyConfig::_ReadRF(uint32 path, uint8 rfRegister)
{
	if (path >= kRfPathCount)
		return 0xFFFFFFFF;

	uint16 base = kBBRegPathBase[path];

	// Write the read command (address only, data = 0)
	uint32 rfCmd = (uint32)rfRegister << 20;
	fRegisterIO->Write32(base + kRegRFPara, rfCmd);

	// Read the result
	snooze(10);  // Brief delay for RF register access to complete
	return fRegisterIO->Read32(base + kRegRFReadData) & 0x000FFFFF;
}


/*! Determine which frequency band a channel belongs to.
    Channels 1-14 are 2.4 GHz, channels 36+ are 5 GHz.
*/
ChannelBand
RTL8814AUPhyConfig::_BandForChannel(uint8 channel)
{
	if (channel <= 14)
		return kBand2_4GHz;
	return kBand5GHz;
}

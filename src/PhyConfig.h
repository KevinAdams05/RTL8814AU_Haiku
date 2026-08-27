/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * PhyConfig.h — PHY/RF/BB configuration for the RTL8814AU.
 *
 * Manages the 4 independent RF/baseband paths (A, B, C, D):
 *   - Baseband register initialization from compiled-in tables
 *   - Per-path RF transceiver configuration
 *   - IQ calibration (TX and RX)
 *   - Channel switching (frequency, bandwidth, band)
 *   - TX power configuration (per-path, per-channel from EFUSE)
 *
 * The RTL8814AU has 4 RF paths at separate register bases:
 *   Path A: 0x2800, Path B: 0x2C00, Path C: 0x3800, Path D: 0x3C00
 *
 * Reference: rtl8814a_phycfg.c, hal/phydm/rtl8814a/ in the
 * ulli-kroll/rtl8814au driver.
 */
#ifndef RTL8814AU_PHY_CONFIG_H
#define RTL8814AU_PHY_CONFIG_H


#include <SupportDefs.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;
class RTL8814AUEfuseReader;


// Register table entry — address/value pair used for BB and RF initialization.
// These are compiled-in tables derived from the reference driver's
// halhwimg8814a_bb.c and halhwimg8814a_rf.c.
struct PhyRegEntry {
	uint32	address;
	uint32	value;
};


// TX power index per path per channel group.
// The EFUSE stores base power indices for each path and channel group.
// There are 14 channel groups total (5 for 2.4 GHz, 9 for 5 GHz).
static const uint32 kTxPwrGroupCount2G = 5;
static const uint32 kTxPwrGroupCount5G = 14;
static const uint32 kTxPwrGroupCountTotal
	= kTxPwrGroupCount2G + kTxPwrGroupCount5G;


class RTL8814AUPhyConfig {
public:
								RTL8814AUPhyConfig(
									RTL8814AURegisterIO* registerIO,
									RTL8814AUEfuseReader* efuseReader);
								~RTL8814AUPhyConfig();

	// Full PHY initialization — call after firmware is loaded.
	// Configures BB registers, RF transceivers, runs IQ calibration,
	// and sets initial TX power.
	status_t					Initialize();

	// Switch to a new channel. Reconfigures RF synthesizers, reloads
	// AGC tables if crossing band boundaries, and re-applies TX power.
	status_t					SetChannel(uint8 channel,
									ChannelBandwidth bandwidth);

	// Get current operating parameters
	uint8						CurrentChannel() const
									{ return fCurrentChannel; }
	ChannelBandwidth			CurrentBandwidth() const
									{ return fCurrentBandwidth; }
	ChannelBand					CurrentBand() const
									{ return fCurrentBand; }

	// The channel the RF chain is actually on, read from RF register 0x18 of
	// path A rather than from fCurrentChannel.
	//
	// The cached value says where the driver believes it put the chip, which
	// is the thing in question when a frame is submitted successfully and
	// never appears on the air. Expect roughly 0x53195 on channel 149 and
	// 0x13124 on channel 36; the upper digits are all zero across 2.4 GHz.
	uint32						ReadRfChannelRegister(uint32 path);

private:
	// Initialization sub-steps
	status_t					_InitBBRegisters();
	status_t					_InitRFRegisters();
	status_t					_RunIQCalibration();
	status_t					_SetTxPower();

	// Table processing — applies a compiled-in array of register writes
	// to the baseband or RF path registers
	status_t					_ApplyBBTable(const PhyRegEntry* table,
									uint32 count);
	status_t					_ApplyRFTable(uint32 path,
									const PhyRegEntry* table,
									uint32 count);

	// TX power helpers
	uint8						_GetTxPowerIndex(uint32 path,
									uint8 channel);
	status_t					_WriteTxPowerIndex(uint32 path,
									uint8 powerIndex);
	static uint32				_ChannelToGroupIndex(uint8 channel);

	// Bandwidth configuration
	status_t					_SetBandwidth(ChannelBandwidth bandwidth);

	// RF register access — indirect through BB registers
	status_t					_WriteRF(uint32 path, uint8 rfRegister,
									uint32 value);
	uint32						_ReadRF(uint32 path, uint8 rfRegister);

	// Reprogram the baseband for a 2.4 <-> 5 GHz change.  Separate from
	// SetChannel because it is only legal on an actual band change, and
	// runs with the demodulator clocks gated.
	status_t					_SwitchBand(ChannelBand band);

	// Route the RF pins to the dongle's external LNA/PA and antenna
	// switch for the given band.  Genuinely per-band: the 2.4 GHz
	// routing cannot receive 5 GHz.
	void						_SetRfePinmux(ChannelBand band);

	// Channel helpers
	static ChannelBand			_BandForChannel(uint8 channel);

	// Per-channel-group baseband/RF programming.  The 5 GHz band is not
	// one band to the hardware: the fc_area filter, the RF synthesizer's
	// band-select bits and the AGC table all step at sub-band boundaries.
	static uint32				_FcAreaForChannel(uint8 channel);
	static uint32				_RfModAgForChannel(uint8 channel);
	static uint32				_AgcTableForChannel(uint8 channel);

	RTL8814AURegisterIO*		fRegisterIO;
	RTL8814AUEfuseReader*		fEfuseReader;

	uint8						fCurrentChannel;
	ChannelBandwidth			fCurrentBandwidth;
	ChannelBand					fCurrentBand;
	bool						fInitialized;

	// TX power indices read from EFUSE — per path, per channel group
	uint8						fTxPowerIndex[kRfPathCount]
									[kTxPwrGroupCountTotal];
};


#endif	// RTL8814AU_PHY_CONFIG_H

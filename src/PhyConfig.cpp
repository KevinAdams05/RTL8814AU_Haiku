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

	// Populate the per-channel TX power table from EFUSE.  This has to
	// happen here, before anything calls SetChannel, because SetChannel
	// re-applies power for the new channel out of that table — and an
	// unpopulated table is all zeros, so every channel change was quietly
	// writing power index 0 over the perfectly good power the cold-start
	// replay had just programmed.  Nothing transmitted at any real power
	// after the first join.
	status = _SetTxPower();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": TX power setup failed: %s\n",
			strerror(status));
		return status;
	}

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

	// Reprogram the baseband for the new band before touching the
	// synthesizer, so the demodulator is never running against a
	// half-configured band.
	if (bandChanged) {
		status_t status = _SwitchBand(newBand);
		if (status != B_OK)
			return status;
	}

	// The fc_area filter steps at sub-band boundaries, not at the
	// 2.4/5 GHz boundary, so it is programmed per channel.
	fRegisterIO->MaskedWrite32(kRegBBFcArea, kBBFcAreaMask,
		_FcAreaForChannel(channel));

	// Configure the RF synthesizer on each path for the new frequency.
	// The channel number goes in the low byte of RF register 0x18, but it
	// is only half the story: bits 18:16 and 9:8 carry a band-select code
	// that is all zeros for 2.4 GHz and non-zero for each 5 GHz sub-band.
	// Writing a bare channel number therefore tuned 2.4 GHz correctly and
	// left the synthesizer on the wrong band for every 5 GHz channel,
	// which is why no 5 GHz frame was ever received.  Read-modify-write,
	// because _SetBandwidth keeps the filter bandwidth in bits 11:10 of
	// this same register.
	uint32 channelValue = ((uint32)channel | _RfModAgForChannel(channel))
		& kRfChannelBandMask;
	for (uint32 path = 0; path < kRfPathCount; path++) {
		uint32 rfValue = _ReadRF(path, kRfRegChannelStandalone);
		rfValue = (rfValue & ~kRfChannelBandMask) | channelValue;

		status_t status = _WriteRF(path, kRfRegChannelStandalone, rfValue);
		if (status != B_OK)
			return status;
	}

	// The 5 GHz AGC table is selected per sub-band.  2.4 GHz selects
	// table 0, which _SwitchBand already did.
	if (newBand == kBand5GHz) {
		fRegisterIO->MaskedWrite32(kRegBBAgcTableSelect,
			kBBAgcTableSelectMask, _AgcTableForChannel(channel));
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

	// Read back the registers that decide whether this band can actually
	// transmit and receive, so a band switch can be checked rather than
	// assumed.  5 GHz receive works and 5 GHz transmit does not, and the
	// difference has to be visible in one of these.
	//
	// RF register 0x18 carries the channel number together with a
	// band-select code, so it is the one value that proves the RF chain
	// agrees with the MAC about where it is: expect roughly 0x13124 on
	// channel 36 and 0x53195 on channel 149, all-zero in the upper digits
	// across 2.4 GHz.  The RFE pinmux is the antenna routing -- rfe_type 20
	// wants 0x77777777 for 2.4 GHz and 0x54775477 for 5 GHz -- and TX power
	// is included because a correct-looking chain that transmits at zero
	// power looks exactly like a chain that does not transmit at all.
	if (bandChanged || newBand == kBand5GHz) {
		dprintf(RTL8814AU_DRIVER_NAME ": [band %s ch%u] "
			"RFE A=0x%08x B=0x%08x C=0x%08x D=0x%08x fc_area=0x%08x\n",
			newBand == kBand5GHz ? "5G" : "2.4G", channel,
			(unsigned)fRegisterIO->Read32(kRegBBRfePinmux0),
			(unsigned)fRegisterIO->Read32(kRegBBRfePinmuxPathB),
			(unsigned)fRegisterIO->Read32(kRegBBRfePinmuxPathC),
			(unsigned)fRegisterIO->Read32(kRegBBRfePinmuxPathD),
			(unsigned)fRegisterIO->Read32(kRegBBFcArea));
		dprintf(RTL8814AU_DRIVER_NAME ": [band %s ch%u] "
			"RF0x18 A=0x%05x B=0x%05x C=0x%05x D=0x%05x  "
			"txpwr A=%u B=%u C=%u D=%u\n",
			newBand == kBand5GHz ? "5G" : "2.4G", channel,
			(unsigned)_ReadRF(0, kRfRegChannelStandalone),
			(unsigned)_ReadRF(1, kRfRegChannelStandalone),
			(unsigned)_ReadRF(2, kRfRegChannelStandalone),
			(unsigned)_ReadRF(3, kRfRegChannelStandalone),
			(unsigned)_GetTxPowerIndex(0, channel),
			(unsigned)_GetTxPowerIndex(1, channel),
			(unsigned)_GetTxPowerIndex(2, channel),
			(unsigned)_GetTxPowerIndex(3, channel));
	}

	return B_OK;
}


// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------


/*! Reprogram the baseband for a 2.4 <-> 5 GHz change.

    Tuning the synthesizer is not enough to change band.  CCK does not
    exist above 2.4 GHz, the demodulator has to be told so, and the AGC
    gain curves differ because the RF front end has different gain and
    noise characteristics per band.  Mirrors the reference driver's
    PHY_SwitchWirelessBand8814A, minus two deliberate omissions noted
    below.

    Runs with the CCK/OFDM clocks gated, so the blocks are never live
    against a half-programmed band.
*/
status_t
RTL8814AUPhyConfig::_SwitchBand(ChannelBand band)
{
	dprintf(RTL8814AU_DRIVER_NAME ": switching band to %s\n",
		band == kBand5GHz ? "5 GHz" : "2.4 GHz");

	// Deliberately NOT bracketing this with the 0x1000[16] CCK/OFDM clock
	// gate that the reference driver drops and restores around a band
	// switch.  On this chip that bit is BIT0 of byte 0x1002, and BB-region
	// writes only land while 0x1002 reads 0x03 (FEN_BBRSTB together with
	// FEN_BB_GLB_RSTn) — see notes/rtl8814au/05-the-bb-write-lock.md, where
	// the same clock-gate pattern is recorded as a false lead that cost a
	// lot of time.  Gating the clock here would leave 0x1002 at 0x02 for
	// exactly the window in which every write below happens, and they
	// would all be silently dropped.
	// Route the RF front end for this band first: everything below is
	// demodulator configuration, and demodulating a band the antenna
	// path cannot deliver is pointless.
	_SetRfePinmux(band);

	if (band == kBand5GHz) {
		// Tell the MAC that CCK is not valid up here, but leave the CCK
		// transmitter reachable — the reference driver keeps CCK TX
		// available even with CCK switched off.
		fRegisterIO->Write8(kRegBBCckCheck, kBBCckCheck5GHz);
		fRegisterIO->MaskedWrite32(kRegBBCckTxOnly, kBBCckTxOnly5GHz,
			kBBCckTxOnly5GHz);

		// OFDM only.  Leaving CCK demodulation enabled in 5 GHz is one
		// of the ways this band silently receives nothing.
		fRegisterIO->MaskedWrite32(kRegBBOfdmCckEn,
			kBBOfdmCckEnOfdm | kBBOfdmCckEnCck, kBBOfdmCckEnOfdm);
	} else {
		fRegisterIO->MaskedWrite32(kRegBBAgcTableSelect,
			kBBAgcTableSelectMask, 0);

		fRegisterIO->MaskedWrite32(kRegBBOfdmCckEn,
			kBBOfdmCckEnOfdm | kBBOfdmCckEnCck,
			kBBOfdmCckEnOfdm | kBBOfdmCckEnCck);

		fRegisterIO->Write8(kRegBBCckCheck, 0);
		fRegisterIO->MaskedWrite32(kRegBBCckTxOnly, kBBCckTxOnly5GHz, 0);
	}

	// Switch LNA mode and reload the band's AGC gain curve.  Both predate
	// this function and are kept as they were, since 2.4 GHz works.
	uint32 lnaValue = (band == kBand5GHz) ? 0x00001 : 0x00000;
	for (uint32 path = 0; path < kRfPathCount; path++)
		_WriteRF(path, kRfRegLNA, lnaValue);

	if (band == kBand5GHz) {
		dprintf(RTL8814AU_DRIVER_NAME ": reloading AGC tables for "
			"5 GHz\n");
		_ApplyBBTable(kAGCTable5G, kAGCTable5GCount);
	} else {
		dprintf(RTL8814AU_DRIVER_NAME ": reloading AGC tables for "
			"2.4 GHz\n");
		_ApplyBBTable(kAGCTable2G, kAGCTable2GCount);
	}

	// The TX path register needs one band-dependent bit.
	//
	// This function used to leave kRegBBTxPath alone entirely, on the
	// grounds that the reference programs generic per-band values while ours
	// are derived from this adapter's EFUSE, so overwriting it would regress
	// the working 2.4 GHz path.  That reasoning still holds for the register
	// as a whole -- which chains transmit is a property of how a dongle is
	// wired, not of the band -- but bit 5 is different: a usbmon capture of
	// the vendor driver shows it set for 2.4 GHz and clear for 5 GHz on one
	// and the same adapter, 0x1000002F against 0x1000000F.
	//
	// Leaving it set in 5 GHz is a strong candidate for why 5 GHz receives
	// perfectly well and transmits nothing the access point ever answers.
	// So flip just that bit and leave every other chain-selection bit as the
	// EFUSE-derived value put it.
	fRegisterIO->MaskedWrite32(kRegBBTxPath, kBBTxPath2_4GHzBit,
		band == kBand5GHz ? 0 : kBBTxPath2_4GHzBit);

	// Still left alone on purpose: kRegBBCckRxPath.  The vendor changes it
	// per band too (0x45FF800C against 0x4FFF800C) but CCK does not exist in
	// 5 GHz, so the CCK receive path cannot be what stops 5 GHz transmit,
	// and ours is tuned for this adapter.

	return B_OK;
}


/*! Route the RF pins for a band.

    These registers connect the chip's RF pins to the dongle's external
    LNA, PA and antenna switch, and the correct routing differs per band —
    the 2.4 GHz routing physically cannot deliver 5 GHz to the receiver.
    Read back at runtime, our dongle sits at the 2.4 GHz value, so this
    had to be programmed before 5 GHz could receive anything.

    Our EFUSE reports rfe_type 20, which lands in the reference driver's
    default branch: one word repeated across the paths, differing only by
    band.  Note the reference's 2.4 GHz default case deliberately leaves
    path D alone, and we match that.

    Writing these registers used to provoke a device check-sum error and a
    USB disconnect, which is why they were long left untouched.  That was
    before the BB unlock was understood — BB-region writes were being
    dropped entirely then.  With 0x1002 reading 0x03 they land cleanly.
*/
void
RTL8814AUPhyConfig::_SetRfePinmux(ChannelBand band)
{
	bool is5GHz = (band == kBand5GHz);
	uint32 pinmux = is5GHz ? kRfePinmux5GHz : kRfePinmux2_4GHz;

	fRegisterIO->Write32(kRegBBRfePinmux0, pinmux);
	fRegisterIO->Write32(kRegBBRfePinmuxPathB, pinmux);
	fRegisterIO->Write32(kRegBBRfePinmuxPathC, pinmux);

	// Path D is written on **both** transitions.
	//
	// It used to be written only when switching to 5 GHz, on the reading
	// that 2.4 GHz leaves path D untouched.  That holds only for a radio
	// that has never been to 5 GHz: once it has, path D keeps the 5 GHz
	// routing for good, and a readback on 2.4 GHz channel 1 showed exactly
	// that -- A, B and C at 0x77777777 with D still at 0x54775477.
	//
	// The consequence was worse than a wrong register. After any 5 GHz
	// excursion, including a failed join, 2.4 GHz reception was dead: scans
	// swept all 42 channels and heard nothing at all, and only a reboot
	// fixed it, because only a reboot re-ran the PHY init that sets path D
	// correctly in the first place.
	// Path D takes its own value in 5 GHz; in 2.4 GHz it matches the rest.
	fRegisterIO->Write32(kRegBBRfePinmuxPathD,
		is5GHz ? kRfePinmux5GHzPathD : pinmux);

	fRegisterIO->MaskedWrite32(kRegBBRfePinmuxCoex, kBBRfePinmuxCoexMask,
		is5GHz ? kRfePinmuxCoex5GHz : kRfePinmuxCoex2_4GHz);
}


/*! fc_area filter setting for a channel.  Steps at 5 GHz sub-band
    boundaries; the last case covers all of 2.4 GHz.
*/
uint32
RTL8814AUPhyConfig::_FcAreaForChannel(uint8 channel)
{
	if (channel >= 36 && channel <= 48)
		return 0x494 << 17;
	if (channel >= 50 && channel <= 64)
		return 0x453 << 17;
	if (channel >= 100 && channel <= 116)
		return 0x452 << 17;
	if (channel >= 118)
		return 0x412 << 17;

	return 0x96a << 17;
}


/*! RF synthesizer band-select code for a channel, already positioned for
    kRfRegChannelStandalone.  Zero across 2.4 GHz, which is exactly why
    writing a bare channel number worked there and nowhere else.
*/
uint32
RTL8814AUPhyConfig::_RfModAgForChannel(uint8 channel)
{
	if (channel >= 36 && channel <= 64)
		return kRfModAgBand1;
	if (channel >= 100 && channel <= 140)
		return kRfModAgBand3;
	if (channel > 140)
		return kRfModAgBand4;

	return kRfModAg2_4GHz;
}


/*! AGC table index for a 5 GHz channel.  Only meaningful in 5 GHz;
    2.4 GHz uses table 0.
*/
uint32
RTL8814AUPhyConfig::_AgcTableForChannel(uint8 channel)
{
	if (channel >= 36 && channel <= 64)
		return 1;
	if (channel >= 100 && channel <= 144)
		return 2;
	if (channel >= 149)
		return 3;

	return 0;
}



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
	// The power-gain block is 42 bytes per path from kEfuseTxPwrBase: 11
	// bytes of 2.4 GHz base indices (six CCK groups then five BW40 groups),
	// deltas, then 14 bytes of 5 GHz base indices at +18.  The old offsets
	// (0x020 and 0x060) landed mid-block on the delta bytes, so most groups
	// read 0xEE and fell through to defaults, discarding this dongle's
	// actual calibration.
	for (uint32 path = 0; path < kRfPathCount; path++) {
		uint16 pathBase = kEfuseTxPwrBase + path * kEfuseTxPwrPathStride;

		// The five 2.4 GHz BW40 groups follow the six CCK groups.
		uint16 offset2G = pathBase + kTxPwrGroupCount2G + 1;
		for (uint32 group = 0; group < kTxPwrGroupCount2G; group++) {
			if (offset2G + group < kEfuseMapSize)
				fTxPowerIndex[path][group] = efuseMap[offset2G + group];
			else
				fTxPowerIndex[path][group] = kTxPwrDefault2G;
		}

		uint16 offset5G = pathBase + kEfuseTxPwr5GInPath;
		for (uint32 group = 0; group < kTxPwrGroupCount5G; group++) {
			uint32 index = kTxPwrGroupCount2G + group;
			if (offset5G + group < kEfuseMapSize)
				fTxPowerIndex[path][index] = efuseMap[offset5G + group];
			else
				fTxPowerIndex[path][index] = kTxPwrDefault5G;
		}
	}

	// Validate every index we just read.  An unprogrammed EFUSE byte reads
	// 0xFF and a wrong offset commonly reads 0x00, and neither is a usable
	// gain index — 0x00 in particular means no output at all.  Channel 149
	// came back 0 on all four paths, which is why the first 5 GHz
	// association attempt transmitted an auth request the access point
	// could not possibly hear.  The reference driver validates these the
	// same way and falls back to its per-band defaults.
	for (uint32 path = 0; path < kRfPathCount; path++) {
		for (uint32 group = 0; group < kTxPwrGroupCountTotal; group++) {
			uint8 index = fTxPowerIndex[path][group];
			if (index != 0 && index <= kTxPwrIndexMax)
				continue;

			bool is5GHz = group >= kTxPwrGroupCount2G;
			fTxPowerIndex[path][group] = is5GHz
				? kTxPwrDefault5G : kTxPwrDefault2G;

			dprintf(RTL8814AU_DRIVER_NAME ": TX power path %u group %u "
				"invalid (0x%02x), using default 0x%02x\n",
				(unsigned)path, (unsigned)group, (unsigned)index,
				(unsigned)fTxPowerIndex[path][group]);
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

	// 5 GHz: the EFUSE carries 14 groups, and the 28 channels we support
	// divide into exactly two per group in channel order — which is also
	// how the factory values read back, ascending smoothly across the
	// band.  Derive the group from the channel's position in the list
	// rather than restating the boundaries and risking disagreement.
	for (uint32 i = 0; i < sizeof(kChannelList5G); i++) {
		if (kChannelList5G[i] == channel)
			return kTxPwrGroupCount2G + i / 2;
	}

	// A 5 GHz channel we don't sweep: fall back to the last group.
	return kTxPwrGroupCount2G + kTxPwrGroupCount5G - 1;
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

	// Writes go through this path's 3-wire LSSI register, with the RF
	// register address in bits 27:20 and the data in bits 19:0.
	//
	// This used to target kBBRegPathBase[path] + 0x1C, which is inside the
	// read window and is not a write interface at all: 0x281C for path A.
	// Every RF write in the driver was therefore discarded, so the chip
	// never actually changed channel and stayed wherever the init tables
	// left it.  2.4 GHz appeared to work only because the test network
	// happened to sit on that channel.
	uint32 command = (((uint32)rfRegister << kRfAddressShift)
		| (value & kRfDataMask)) & kRfCommandMask;

	return fRegisterIO->Write32(kRfLssiWriteReg[path], command);
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

	// Reads come from a direct-mapped window, where each RF register has
	// its own 32-bit slot.  No command write and no wait: the previous
	// implementation wrote a command to base + 0x20 and read base + 0x24,
	// which is simply the slot for RF register 9, and so returned a
	// constant unrelated value.
	uint16 address = kBBRegPathBase[path] + (uint16)rfRegister * 4;

	return fRegisterIO->Read32(address) & kRfDataMask;
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

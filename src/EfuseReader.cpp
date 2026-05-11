/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * EfuseReader.cpp — EFUSE one-time-programmable memory reader for RTL8814AU.
 *
 * Physical EFUSE is 1024 bytes stored in a compressed format. This module
 * reads the raw data and decodes it into a 512-byte logical map. The map
 * contains:
 *   - MAC address (offset 0x0D8, 6 bytes — USB variant)
 *   - Antenna TX/RX path config (offset 0x00E)
 *   - RFE type (offset 0x010) — determines PA/LNA wiring
 *   - TX power calibration tables (2.4 GHz and 5 GHz)
 *   - Thermal meter calibration
 *   - Crystal calibration
 *   - Regulatory channel plan
 *
 * The compressed format uses a header byte + data scheme:
 *   - If header == 0xFF: end of EFUSE data
 *   - If (header & 0x1F) == 0x0F: extended header (2-byte)
 *   - Otherwise: 1-byte header with 4-byte data group address + word mask
 *
 * Reference: efuse.c and rtl8814a_hal_init.c:Hal_EfuseReadEFuse8814A()
 * in the ulli-kroll/rtl8814au driver.
 */

#include "EfuseReader.h"

#include <string.h>

#include <KernelExport.h>

#include "RegisterIO.h"


RTL8814AUEfuseReader::RTL8814AUEfuseReader(RTL8814AURegisterIO* registerIO)
	:
	fRegisterIO(registerIO),
	fMapValid(false)
{
	// Initialize map to 0xFF (unprogrammed EFUSE reads as all-ones)
	memset(fMap, 0xFF, sizeof(fMap));
}


RTL8814AUEfuseReader::~RTL8814AUEfuseReader()
{
}


// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------


/*! Read the EFUSE contents from hardware and decode into the logical map.
    This involves reading 1024 bytes of physical EFUSE one byte at a time
    through the indirect access register, then decoding the compressed
    format into the 512-byte logical map.

    \return B_OK on success, or an error code.
*/
status_t
RTL8814AUEfuseReader::ReadEfuseMap()
{
	dprintf(RTL8814AU_DRIVER_NAME ": reading EFUSE map\n");

	// Reset map to 0xFF
	memset(fMap, 0xFF, sizeof(fMap));
	fMapValid = false;

	status_t status = _DecodeMap();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": EFUSE decode failed: %s\n",
			strerror(status));
		return status;
	}

	fMapValid = true;

	// Dump key EFUSE regions for debugging
	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map[0x00E] antenna=%02x\n",
		fMap[kEfuseAntennaConfig]);
	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map[0x010] rfe=%02x\n",
		fMap[kEfuseRfeType]);
	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map[0x100] thermal=%02x\n",
		fMap[kEfuseThermalMeter]);
	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map[0x0D8..0x0DD] MAC: "
		"%02x:%02x:%02x:%02x:%02x:%02x\n",
		fMap[kEfuseMacAddr], fMap[kEfuseMacAddr + 1],
		fMap[kEfuseMacAddr + 2], fMap[kEfuseMacAddr + 3],
		fMap[kEfuseMacAddr + 4], fMap[kEfuseMacAddr + 5]);

	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE map read successfully\n");
	return B_OK;
}


/*! Return pointer to the decoded EFUSE map, or NULL if not yet read. */
const uint8*
RTL8814AUEfuseReader::Map() const
{
	if (!fMapValid)
		return NULL;
	return fMap;
}


/*! Return the RFE (RF Front End) type from EFUSE. Determines PA/LNA
    wiring configuration. Range: 0–6.
*/
uint8
RTL8814AUEfuseReader::RfeType() const
{
	if (!fMapValid)
		return 0;
	return fMap[kEfuseRfeType];
}


/*! Return the antenna configuration byte from EFUSE. Encodes which
    TX and RX paths are connected to antennas.
*/
uint8
RTL8814AUEfuseReader::AntennaConfig() const
{
	if (!fMapValid)
		return 0xFF;
	return fMap[kEfuseAntennaConfig];
}


/*! Return the thermal meter calibration value from EFUSE. Used by the
    PHY engine to compensate TX power drift with temperature changes.
*/
uint8
RTL8814AUEfuseReader::ThermalMeter() const
{
	if (!fMapValid)
		return 0xFF;
	return fMap[kEfuseThermalMeter];
}


/*! Return the channel plan (regulatory domain) from EFUSE. Determines
    which channels are available for use.
*/
uint16
RTL8814AUEfuseReader::ChannelPlan() const
{
	if (!fMapValid)
		return 0xFFFF;
	return (uint16)fMap[kEfuseChannelPlan]
		| ((uint16)fMap[kEfuseChannelPlan + 1] << 8);
}


// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------


/*! Read a single byte from the physical EFUSE via the indirect access
    register. The hardware provides a register-based interface:
      1. Read current EFUSE_CTRL register (preserve mode/control bits)
      2. Set address in bits [17:8]
      3. Clear the valid bit (bit 31) to trigger a new read cycle
      4. Write back to start the read
      5. Poll until the valid bit is set (hardware signals data ready)
      6. Read the data from bits [7:0]

    Reference: ReadEFuseByte() in the Realtek reference driver — the
    read-modify-write preserves control bits while clearing the valid
    bit triggers a fresh EFUSE cell read.

    \param address  Physical EFUSE address (0–1023)
    \param value    Output: the byte read
    \return B_OK on success, B_TIMED_OUT if read didn't complete.
*/
status_t
RTL8814AUEfuseReader::_ReadByte(uint16 address, uint8* value)
{
	// Read current register value to preserve control/mode bits
	uint32 ctrl = fRegisterIO->Read32(kRegEfuseCtrl);

	// Set address field (bits [17:8]), preserving other bits
	ctrl &= ~kEfuseCtrlAddr_Mask;
	ctrl |= (address << kEfuseCtrlAddr_Shift) & kEfuseCtrlAddr_Mask;

	// Clear the valid bit (bit 31) — this triggers a new read cycle
	ctrl &= ~kEfuseCtrlValid;

	fRegisterIO->Write32(kRegEfuseCtrl, ctrl);

	// Poll for valid bit — hardware sets it when data is ready
	status_t status = fRegisterIO->PollFor32(kRegEfuseCtrl,
		kEfuseCtrlValid, kEfuseCtrlValid, 100, 10);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": EFUSE read timeout at "
			"address 0x%03x\n", address);
		*value = 0xFF;
		return B_TIMED_OUT;
	}

	// Extract the data byte from bits [7:0]
	uint32 result = fRegisterIO->Read32(kRegEfuseCtrl);
	*value = (uint8)(result & kEfuseCtrlData_Mask);
	return B_OK;
}


/*! Decode the compressed physical EFUSE into the logical map. The format
    uses a tag-length-value encoding where each entry specifies a 4-byte
    group offset and a word enable mask indicating which of the 4 bytes
    in the group are present.

    Format for each entry:
      Standard header (1 byte):
        [7:4] = offset (group address, multiply by 8 for map offset)
        [3:0] = word enable mask (bit=0 means word is present)
      Extended header (2 bytes, when [3:0] == 0x0F):
        byte1[7:4] = offset high nibble
        byte2[7:4] = offset low nibble
        byte2[3:0] = word enable mask

    A header byte of 0xFF signals end of EFUSE data.
*/
status_t
RTL8814AUEfuseReader::_DecodeMap()
{
	uint16 physAddr = 0;

	while (physAddr < kEfuseTotalSize) {
		uint8 header;
		status_t status = _ReadByte(physAddr, &header);
		if (status != B_OK)
			return status;

		// 0xFF = end of EFUSE data
		if (header == 0xFF)
			break;

		physAddr++;

		uint16 mapOffset;
		uint8 wordEnable;

		if ((header & 0x1F) == 0x0F) {
			// Extended header format: 2-byte header
			uint8 header2;
			status = _ReadByte(physAddr, &header2);
			if (status != B_OK)
				return status;
			physAddr++;

			// Offset = header[7:5] in bits [2:0], header2[7:4] in bits [6:3]
			mapOffset = (uint16)(((header & 0xE0) >> 5)
				| ((header2 & 0xF0) >> 1));
			mapOffset *= 8;
			wordEnable = header2 & 0x0F;
		} else {
			// Standard 1-byte header
			mapOffset = (uint16)((header >> 4) & 0x0F);
			mapOffset *= 8;
			wordEnable = header & 0x0F;
		}

		// Read the data words. Each bit in wordEnable corresponds to a
		// 2-byte word. A 0 bit means the word IS present (inverted logic).
		for (int32 wordIndex = 0; wordIndex < 4; wordIndex++) {
			if ((wordEnable & (1 << wordIndex)) == 0) {
				// This word is present — read 2 bytes
				uint16 targetOffset = mapOffset + wordIndex * 2;
				if (targetOffset + 1 < kEfuseMapSize) {
					status = _ReadByte(physAddr, &fMap[targetOffset]);
					if (status != B_OK)
						return status;
					physAddr++;

					status = _ReadByte(physAddr, &fMap[targetOffset + 1]);
					if (status != B_OK)
						return status;
					physAddr++;
				} else {
					// Out of bounds — skip these bytes
					physAddr += 2;
				}
			}
		}
	}

	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE decoded, read %" B_PRIu16
		" physical bytes\n", physAddr);
	return B_OK;
}

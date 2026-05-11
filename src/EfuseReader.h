/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * EfuseReader.h — EFUSE (one-time-programmable memory) access for RTL8814AU.
 *
 * The RTL8814AU has 1024 bytes of physical EFUSE (in 2 banks) that stores
 * factory-programmed calibration data:
 *   - MAC address
 *   - TX power tables (per-path, per-channel, per-band)
 *   - Antenna configuration (which RF paths are connected)
 *   - RFE (RF Front End) type (PA/LNA configuration)
 *   - Thermal meter calibration value
 *   - Crystal oscillator calibration
 *   - Regulatory channel plan
 *
 * The physical EFUSE is encoded in a compressed format. This module reads
 * the raw EFUSE data and decodes it into a 512-byte logical map that other
 * modules can index into directly.
 */
#ifndef RTL8814AU_EFUSE_READER_H
#define RTL8814AU_EFUSE_READER_H


#include <SupportDefs.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;


class RTL8814AUEfuseReader {
public:
								RTL8814AUEfuseReader(
									RTL8814AURegisterIO* registerIO);
								~RTL8814AUEfuseReader();

	// Read the full EFUSE map from hardware. Must be called before
	// any Map() access. Returns B_OK on success.
	status_t					ReadEfuseMap();

	// Access the decoded EFUSE map. Returns NULL if not yet read.
	const uint8*				Map() const;

	// Convenience accessors for commonly-needed fields
	uint8						RfeType() const;
	uint8						AntennaConfig() const;
	uint8						ThermalMeter() const;
	uint16						ChannelPlan() const;

private:
	// Read a single byte from the physical EFUSE at the given address
	status_t					_ReadByte(uint16 address, uint8* value);

	// Decode the compressed physical EFUSE into the logical map
	status_t					_DecodeMap();

	RTL8814AURegisterIO*		fRegisterIO;
	uint8						fMap[kEfuseMapSize];
	bool						fMapValid;
};


// EFUSE access registers
static const uint16 kRegEfuseCtrl		= 0x0030;
static const uint16 kRegEfuseTest		= 0x0034;
static const uint16 kRegEfuseClkCtrl	= 0x0038;

// EFUSE control bits
static const uint32 kEfuseCtrlValid		= (1 << 31);	// Data valid
static const uint32 kEfuseCtrlAddr_Shift = 8;			// Address field
static const uint32 kEfuseCtrlAddr_Mask	= 0x03FF00;		// 10-bit address
static const uint32 kEfuseCtrlData_Mask	= 0x000000FF;	// 8-bit data


#endif	// RTL8814AU_EFUSE_READER_H

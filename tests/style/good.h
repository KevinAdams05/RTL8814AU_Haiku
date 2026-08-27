/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * good.h -- a style-checker fixture that must produce zero findings.
 */
#ifndef RTL8814AU_GOOD_H
#define RTL8814AU_GOOD_H


#include <SupportDefs.h>


static const uint32 kGoodFrameCapacity = 64;


class GoodExample {
public:
								GoodExample();
								~GoodExample();

	status_t					SubmitFrame(const uint8* frame,
									uint32 frameLength);

private:
	status_t					_Validate(uint32 frameLength) const;

	uint32						fFrameCount;
	bool						fRunning;
};


#endif	// RTL8814AU_GOOD_H

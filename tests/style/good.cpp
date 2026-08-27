/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * good.cpp -- a style-checker fixture that must produce zero findings.
 *
 * Doxygen continuation lines in this tree are indented with four spaces
 * rather than a tab, which is why the indentation rule has to know when it
 * is looking at a comment body.
 */

#include "good.h"

#include <new>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>


static const uint32 kGoodMaximumFrameSize = 2400;


/*! Build one example object.
    The continuation line above is space-indented on purpose: it is the
    doxygen form used above every public method in src/.
*/
GoodExample::GoodExample()
	:
	fFrameCount(0),
	fRunning(false)
{
}


GoodExample::~GoodExample()
{
}


/*! Queue one frame for transmission.

    \param frame        The 802.11 frame, including its header.
    \param frameLength  Length of \a frame in bytes.
    \return B_OK, or B_BAD_VALUE when the frame is unusable.
*/
status_t
GoodExample::SubmitFrame(const uint8* frame, uint32 frameLength)
{
	if (frame == NULL)
		return B_BAD_VALUE;

	status_t status = _Validate(frameLength);
	if (status != B_OK)
		return status;

	uint8* copy = new(std::nothrow) uint8[frameLength];
	if (copy == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": out of memory for %" B_PRIu32
			" bytes\n", frameLength);
		return B_NO_MEMORY;
	}

	memcpy(copy, frame, frameLength);
	fFrameCount++;
	delete[] copy;

	return B_OK;
}


/*! Reject frames the hardware cannot describe in a TX descriptor. */
status_t
GoodExample::_Validate(uint32 frameLength) const
{
	if (frameLength == 0 || frameLength > kGoodMaximumFrameSize)
		return B_BAD_VALUE;

	if ((fFrameCount & 0x01) != 0)
		return B_OK;

	switch (frameLength) {
		case 0:
			return B_BAD_VALUE;

		default:
			break;
	}

	return B_OK;
}

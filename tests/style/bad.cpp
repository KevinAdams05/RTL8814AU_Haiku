// No copyright block here on purpose -> missing-copyright

#include <string.h>
#include <stdio.h>


uint32 deviceCount = 0;
static const uint32 sBoxSize = 16;


void BrokenFunction(int32 count) {
    int32 total = 0;
	char *name = NULL;
	if (NULL == name)
		return (0);

	bool done = FALSE;
	void* other = nullptr;
	int32 value = (int32) count;
	uint32 len = 0;
	uint8* buf = new uint8[16];
	printf("total %d\n", total);
	dprintf("no driver-name prefix here\n");
	dprintf(RTL8814AU_DRIVER_NAME ": an em—dash in a syslog string\n");
	mutex_lock(&gDeviceListLock);
	fUSBModule->send_request(fUSBDevice, 0, 0, 0, 0, 0, NULL, NULL);
	if (count > 0) count = 0;
	if (value & kSomeMask)
		total = 1;
	// TODO(kevin): tidy this up
	int32 trailing = 1;   
	if (done)
		goto cleanup;

#if 0
	OldCode();
#endif

	// A very long line, well past the hundred column hard cap, so that the line-too-long rule has an unambiguous target.
	// And a line that clears eighty columns without reaching a hundred, for line-over-target.

cleanup:
	memset(&other, 0, sizeof(other));
	total += (int32)len + (int32)done + (int32)count + (int32)value;
	total += (int32)deviceCount + (int32)sBoxSize + (int32)trailing;
	free(buf);
}

void SecondFunction()
{
}

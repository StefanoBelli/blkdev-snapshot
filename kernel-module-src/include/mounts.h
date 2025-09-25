#ifndef MOUNTS_H
#define MOUNTS_H

#include <get-loop-backing-file.h>

enum mountinfo_device_type : char {
	MOUNTINFO_DEVICE_TYPE_BLOCK = 0,
	MOUNTINFO_DEVICE_TYPE_LOOP
};

struct mountinfo {
	union {
		dev_t bdevt;
		char lo_fname[__MY_LO_NAME_SIZE];
	} device;

	enum mountinfo_device_type type;
};

int setup_mounts(void);
void destroy_mounts(void);

#endif

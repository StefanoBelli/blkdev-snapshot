#ifndef MOUNTS_H
#define MOUNTS_H

#include <uapi/linux/major.h>
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

static inline void from_block_device_to_mountinfo(
		struct mountinfo *mountinfo,
		const struct block_device* bdev) {

	mountinfo->device.bdevt = bdev->bd_dev;
	mountinfo->type = MAJOR(bdev->bd_dev) == LOOP_MAJOR;

	if(mountinfo->type == MOUNTINFO_DEVICE_TYPE_LOOP) {
		char* lbf_ptr = get_loop_backing_file(bdev);
		strscpy(mountinfo->device.lo_fname, lbf_ptr, __MY_LO_NAME_SIZE);
	}
}

int setup_mounts(void);
void destroy_mounts(void);

#endif

#ifndef GET_LOOP_BACKING_FILE_H
#define GET_LOOP_BACKING_FILE_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,74)
#error you should upgrade your system (unknown struct loop_device def)
#endif

//provides struct block_device (for get_loop_backing_file macro)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
#include <linux/fs.h>
#else
#include <linux/blk_types.h>
#endif

//provides struct gendisk (for get_loop_backing_file macro)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
#include <linux/genhd.h>
#else
#include <linux/blkdev.h>
#endif

// cannot do direct offset calculation since compiler can apply
// padding on structs

// trying to avoid polluting the "global" namespace

#define __MY_LO_NAME_SIZE 64

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0)

struct __my_loop_device {
	int 	unused_0;
	loff_t 	unused_1;
	loff_t 	unused_2;
	int 	unused_3;
	char 	lo_file_name[__MY_LO_NAME_SIZE];
};

#elif KERNEL_VERSION(5,16,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)

struct __my_loop_device {
	int 		unused_0;
	atomic_t 	unused_1;
	loff_t 		unused_2;
	loff_t 		unused_3;
	int 		unused_4;
	char 		lo_file_name[__MY_LO_NAME_SIZE];
};

#elif KERNEL_VERSION(4,2,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(5,16,0)

struct __my_loop_device {
	int 		unused_0;
	atomic_t 	unused_1;
	loff_t 		unused_2;
	loff_t 		unused_3;
	int 		unused_4;
	void 		*unused_5;
	char 		lo_file_name[__MY_LO_NAME_SIZE];
};

#else

struct __my_loop_device {
	int 		unused_0;
	int 		unused_1;
	loff_t 		unused_2;
	loff_t 		unused_3;
	int 		unused_4;
	void 		*unused_5;
	char 		lo_file_name[__MY_LO_NAME_SIZE];
};

#endif

#define get_loop_backing_file(bdev) \
	(((struct __my_loop_device*)bdev->bd_disk->private_data)->lo_file_name)

#endif

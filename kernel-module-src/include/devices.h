#ifndef DEVICES_H
#define DEVICES_H

#include <linux/blk_types.h>

/**
 * Each of these calls in process context
 */
bool setup_devices(void);
void destroy_devices(void);
int register_device(const char*);
int unregister_device(const char*);

//sym is exported (gpl)
unsigned long get_block_magic(const struct block_device*);

#endif
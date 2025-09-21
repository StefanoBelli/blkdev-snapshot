#ifndef DEVICES_H
#define DEVICES_H

#include <linux/blk_types.h>

bool setup_devices(void);
bool destroy_devices(void);
int register_device(const char*);
bool unregister_device(const char*);
bool check_device(const struct block_device*, unsigned long*);

#endif
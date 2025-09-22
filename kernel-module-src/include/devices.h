#ifndef DEVICES_H
#define DEVICES_H

#include <linux/blk_types.h>

bool setup_devices(void);
void destroy_devices(void);
int register_device(const char*);
int unregister_device(const char*);

#endif
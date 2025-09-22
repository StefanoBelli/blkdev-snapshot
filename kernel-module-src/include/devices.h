#ifndef DEVICES_H
#define DEVICES_H

/**
 * Each of these calls in process context
 */
int setup_devices(void);
void destroy_devices(void);
int register_device(const char*);
int unregister_device(const char*);

#endif
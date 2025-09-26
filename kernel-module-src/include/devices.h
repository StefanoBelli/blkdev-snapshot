#ifndef DEVICES_H
#define DEVICES_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <mounts.h>
#include <epoch.h>

struct object_data {
	struct epoch e;
	struct workqueue_struct *wq;
	spinlock_t lock;
};


/**
 * Each of these calls in process context
 */
int setup_devices(void);
void destroy_devices(void);
int register_device(const char*);
int unregister_device(const char*);

// protect with rcu_read_lock() and rcu_read_unlock()
struct object_data *get_device_data(struct mountinfo*);

#endif

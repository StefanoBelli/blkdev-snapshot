#ifndef DEVICES_H
#define DEVICES_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <mounts.h>

#define MNT_FMT_DATE_LEN sizeof("-9999-12-31_23:59:59")

struct epoch {
	int n_currently_mounted;
	char first_mount_date[MNT_FMT_DATE_LEN + 1];
	struct dentry *d_snapdir;
	struct list_lru *cached_blocks;
};

struct object_data {
	bool wq_is_destroyed;
	spinlock_t general_lock;
	spinlock_t wq_destroy_lock;
	struct workqueue_struct *wq;
	struct epoch e;
	char original_dev_name[PATH_MAX];
};

/**
 * Each of these calls in process context
 */
int setup_devices(void);
void destroy_devices(void);
int register_device(const char*);
int unregister_device(const char*);

// --> !!wrap with rcu_read_lock/rcu_read_unlock!!
//"always" does not consider mounting status, only if it is recorded
struct object_data *get_device_data_always(const struct mountinfo*);

#endif

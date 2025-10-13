#ifndef DEVICES_H
#define DEVICES_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <mounts.h>
#include <lru-ng.h>

#define MNT_FMT_DATE_LEN sizeof("-9999-12-31_23:59:59")

struct epoch {
	int n_currently_mounted;
	char first_mount_date[MNT_FMT_DATE_LEN + 1];
	struct path *path_snapdir;
	struct lru_ng *cached_blocks;
};

static inline void destroy_an_epoch(struct epoch* epoch) {
	if(epoch != NULL) {
		if(epoch->path_snapdir != NULL) {
			path_put(epoch->path_snapdir);
		}

		if(epoch->cached_blocks != NULL) {
			lru_ng_cleanup_and_destroy(epoch->cached_blocks);
		}

		kfree(epoch);
	}
}

// - the wq_destroy_lock can be held by the thread which execution path falls in
//   either the first or the second point (see above) or by the fs-implementor kprobe,
//   where the lock is taken prior the queue_work.
//
// - the cleanup_epoch_lock will be held by the fs-implementor using the exported funcs
//   (see include <bdsnap/bdsnap.h>) and by __epoch_event_cb_count_umount in a fine-grain way
//   (around queue_work), this should solve issues involving umount and its flags (in particular
//   MNT_DETACH)
struct object_data {
	bool wq_is_destroyed;
	spinlock_t general_lock;
	spinlock_t wq_destroy_lock;
	spinlock_t cleanup_epoch_lock;
	struct workqueue_struct *wq;
	struct epoch *e;
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
//"always" does not consider 
//device mounting status, only if it is recorded or not
//internal usage, fs snap implementor should not use this
struct object_data *get_device_data_always(const struct mountinfo*);


#endif

#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <uapi/linux/mount.h>

#include <mounts.h>
#include <devices.h>
#include <lru-ng.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
#error your version is not compat (reason: kretprobes hooked funcs)
#endif

/**
 *
 * mount events counting
 *
 */

struct finish_epoch_work {
	struct work_struct work;
	struct path *path;
	struct lru_ng *lru;
};

static void cleanup_epoch_work(struct work_struct *work) {
	struct finish_epoch_work *few = 
		container_of(work, struct finish_epoch_work, work);

	if(few->path != NULL) {
		path_put(few->path);
	}

	if(few->lru != NULL) {
		lru_ng_cleanup_and_destroy(few->lru);
	}

	kfree(few);
}

static void __epoch_event_cb_count_mount(struct epoch* epoch) {
	epoch->n_currently_mounted++;

	if(epoch->n_currently_mounted == 1) {
		struct timespec64 ts;
		struct tm tm;

		ktime_get_real_ts64(&ts);
		time64_to_tm(ts.tv_sec, 0, &tm);

		snprintf(
				epoch->first_mount_date, 
				MNT_FMT_DATE_LEN, 
				"-%04ld-%02d-%02d_%02d:%02d:%02d", 

				tm.tm_year + 1900, 
				tm.tm_mon + 1, 
				tm.tm_mday, 
				tm.tm_hour, 
				tm.tm_min, 
				tm.tm_sec
		);
	}
}

static void __epoch_event_cb_count_umount(struct epoch* epoch) {
	epoch->n_currently_mounted--;

	if(epoch->n_currently_mounted < 0) {
		//this is a bug (?)
		//OR, this can happen if when activate_snapshot 
		//the device is already mounted somewhere
		epoch->n_currently_mounted = 0;
	} else if(epoch->n_currently_mounted == 0) {
		struct object_data *data = 
			container_of(epoch, struct object_data, e);

		struct path *saved_path = data->e.path_snapdir;
		struct lru_ng *saved_lru = data->e.cached_blocks;

		data->e.path_snapdir = NULL;
		data->e.cached_blocks = NULL;

		//we hold the general lock, at this time the wq is destroyed or not
		//but nothing can happen while we have the lock
		if(!data->wq_is_destroyed) {
			struct finish_epoch_work *few = (struct finish_epoch_work*) 
				kmalloc(sizeof(struct finish_epoch_work), GFP_ATOMIC);

			if(few != NULL) {
				few->path = saved_path;
				few->lru = saved_lru;

				INIT_WORK(&few->work, cleanup_epoch_work);
				
				unsigned long flags; //cpu-saved flags
				spin_lock_irqsave(&data->cleanup_epoch_lock, flags);
				queue_work(data->wq, &few->work);
				spin_unlock_irqrestore(&data->cleanup_epoch_lock, flags);
			}
		}
	}
}

static void __do_epoch_event_count(const struct mountinfo* minfo, void (*cb)(struct epoch*)) {
	rcu_read_lock();

	struct object_data *data = get_device_data_always(minfo);
	if(data == NULL) {
		rcu_read_unlock();
		return;
	}

	unsigned long flags; //cpu-saved flags
	spin_lock_irqsave(&data->general_lock, flags);
	cb(&data->e);
	spin_unlock_irqrestore(&data->general_lock, flags);

	rcu_read_unlock();
}

static inline void epoch_count_mount(const struct mountinfo *minfo) {
	__do_epoch_event_count(minfo, __epoch_event_cb_count_mount);
}

static inline void epoch_count_umount(const struct mountinfo *minfo) {
	__do_epoch_event_count(minfo, __epoch_event_cb_count_umount);
}

/**
 * 
 * utils
 *
 */

static inline struct block_device *get_bdev_from_path(const struct path *path) {
	struct block_device *bdev;

	if(path->mnt == NULL || 
			path->mnt->mnt_sb == NULL || 
			(bdev = path->mnt->mnt_sb->s_bdev) == NULL) {
		return NULL;
	}

	return bdev;
}

/*
 *
 * new mount op probe callbacks
 * (starting from kernel version 5.2.0, 
 * mount userspace utility uses this since ???)
 *
 */

#define KRP_NEW_MOUNT_SYMBOL_NAME "do_move_mount"

static int new_mount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *old_path = (struct path *) regs->di;
	struct block_device *bdev = get_bdev_from_path(old_path);

	if(bdev == NULL) {
		return 1;
	}

	char __tmp_oldp[33];
	memset(__tmp_oldp, 0, 33);
	char* oldpath_str = d_path(old_path, __tmp_oldp, 32);

	if(strcmp(oldpath_str, "/") != 0) {
		return 1;
	}

	struct mountinfo *minfo = (struct mountinfo*) krp_inst->data;
	from_block_device_to_mountinfo(minfo, bdev);

	return 0;
}

static int mount_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	if(regs_return_value(regs) != 0) {
		return 0;
	}

	epoch_count_mount((struct mountinfo*) krp_inst->data);
	return 0;
}

/*
 *
 * old mount op probe callbacks, exit handler above
 * (starting from kernel version 5.9.0)
 *
 */

#define KRP_OLD_MOUNT_SYMBOL_NAME "path_mount"

static int old_mount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *path = (struct path*) regs->si;
	unsigned long flags = regs->r10;

	bool is_new_mount = 
		!((flags & (MS_REMOUNT | MS_BIND)) == (MS_REMOUNT | MS_BIND)) &&
		!(flags & MS_REMOUNT) &&
		!(flags & MS_BIND) &&
		!(flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)) &&
		!(flags & MS_MOVE);

	if(!is_new_mount) {
		return 1;
	}

	struct block_device *bdev = get_bdev_from_path(path);
	if(bdev == NULL) {
		return 1;
	}

	struct mountinfo *minfo = (struct mountinfo*) krp_inst->data;
	from_block_device_to_mountinfo(minfo, bdev);

	return 0;
}

/* 
 *
 * umount op probe callbacks
 * (starting from kernel version 5.9.0)
 *
 */

#define KRP_UMOUNT_SYMBOL_NAME "path_umount"

static int umount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *path = (struct path*) regs->di;

	struct block_device *bdev = get_bdev_from_path(path);
	if(bdev == NULL) {
		return 1;
	}

	struct mountinfo *minfo = (struct mountinfo*) krp_inst->data;
	from_block_device_to_mountinfo(minfo, bdev);

	return 0;
}

static int umount_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	if(regs_return_value(regs) != 0) {
		return 0;
	}

	epoch_count_umount((struct mountinfo*) krp_inst->data);
	return 0;
}

/* 
 *
 * kretprobes 
 *
 * either one of krp_new_mount or krp_old_mount will be probed
 * according to the userspace mount utility (or whatever is
 * responsible to initiate mount operation via some kernel 
 * system calls)
 *
 */

static struct kretprobe krp_new_mount = {
	.entry_handler = new_mount_entry_handler,
	.handler = mount_handler,
	.kp.symbol_name = KRP_NEW_MOUNT_SYMBOL_NAME,
	.maxactive = -1,
	.data_size = sizeof(struct mountinfo)
};

static struct kretprobe krp_old_mount = {
	.entry_handler = old_mount_entry_handler,
	.handler = mount_handler,
	.kp.symbol_name = KRP_OLD_MOUNT_SYMBOL_NAME,
	.maxactive = -1,
	.data_size = sizeof(struct mountinfo)
};

static struct kretprobe krp_umount = {
	.entry_handler = umount_entry_handler,
	.handler = umount_handler,
	.kp.symbol_name = KRP_UMOUNT_SYMBOL_NAME,
	.maxactive = -1,
	.data_size = sizeof(struct mountinfo)
};

/*
 *
 * which kretprobes to register and setup/destroy funcs
 *
 */

static struct kretprobe *krps_to_register[] = {
	&krp_new_mount,
	&krp_old_mount,
	&krp_umount
};

static size_t num_krps_to_register = 
	sizeof(krps_to_register) / sizeof(struct kretprobe*);

int setup_mounts(void) {
	return register_kretprobes(krps_to_register, num_krps_to_register);
}

void destroy_mounts(void) {
	unregister_kretprobes(krps_to_register, num_krps_to_register);
}

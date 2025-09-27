#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/major.h>
#include <uapi/linux/mount.h>

#include <mounts.h>
#include <devices.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
#error your version is not compat (reason: kretprobes hooked funcs)
#endif

/**
 *
 * mount events refcounting
 *
 */

static void __epoch_event_cb_count_mount(struct epoch* epoch) {
	epoch->n_currently_mounted++;

	if(epoch->n_currently_mounted == 1) {
		//new epoch starts
		//record date
		//reset ptrs as struct epoch is unique per-device
	}
}

static void __epoch_event_cb_count_umount(struct epoch* epoch) {
	epoch->n_currently_mounted--;

	if(epoch->n_currently_mounted < 0) {
		epoch->n_currently_mounted = 0;
	} else if(epoch->n_currently_mounted == 0) {
		struct object_data *data = 
			container_of(epoch, struct object_data, e);

		//queue_work(data->wq, &work);
	}
}

static void __do_epoch_event_count(const struct mountinfo* minfo, void (*cb)(struct epoch*)) {
	rcu_read_lock();

	struct object_data *data = get_device_data(minfo);
	if(data == NULL) {
		rcu_read_unlock();
		return;
	}

	unsigned long flags; //cpu-saved flags
	spin_lock_irqsave(&data->lock, flags);

	cb(&data->e);

	spin_unlock_irqrestore(&data->lock, flags);
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

static inline void from_block_device_to_mountinfo(
		struct mountinfo *mountinfo,
		const struct block_device* bdev) {

	mountinfo->device.bdevt = bdev->bd_dev;
	mountinfo->type = MAJOR(bdev->bd_dev) == LOOP_MAJOR;

	if(mountinfo->type == MOUNTINFO_DEVICE_TYPE_LOOP) {
		char* lbf_ptr = get_loop_backing_file(bdev);
		memcpy(mountinfo->device.lo_fname, lbf_ptr, __MY_LO_NAME_SIZE);
	}
}

/*
 * new mount op probe callbacks
 * (starting from kernel version 5.2.0, 
 * mount userspace utility uses this since ???)
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
 * old mount op probe callbacks
 * (starting from kernel version 5.9.0)
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
 * umount op probe callbacks
 * (starting from kernel version 5.9.0)
 */

#define KRP_UMOUNT_SYMBOL_NAME "path_umount"

static int umount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *path = (struct path*) regs->di;
	struct block_device *bdev = get_bdev_from_path(path);
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
 * kretprobes 
 *
 * either one of krp_new_mount or krp_old_mount will be probed
 * according to the userspace mount utility (or whatever is
 * responsible to initiate mount operation via some kernel 
 * system calls)
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
 * which kretprobes to register and setup/destroy funcs
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

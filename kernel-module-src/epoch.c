#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/kprobes.h>
#include <linux/version.h>

#include <epoch.h>
#include <get-loop-backing-file.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
#define KRP_MOUNT_SYMBOL_NAME "do_move_mount"
#endif

#define KRP_UMOUNT_SYMBOL_NAME "path_umount"

static struct kretprobe krp_mount;
static struct kretprobe krp_umount;

static struct kretprobe *krps[] = {
	&krp_mount,
	&krp_umount
};

static size_t num_krps = sizeof(krps) / sizeof(struct kretprobe*);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
static int mount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *old_path = (struct path*) regs->di;
	struct vfsmount *vfsm = old_path->mnt;
	struct super_block *sb = vfsm->mnt_sb;

	pr_err("MOUNT ENTRY %d %s\n", sb->s_bdev->bd_dev, sb->s_bdev->bd_disk->disk_name);
	return 0;
}

static int mount_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	pr_err("MOUNT EXIT\n");
	return 0;
}
#endif

static int umount_entry_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	struct path *path = (struct path*) regs->di;
	struct block_device *bd = path->mnt->mnt_sb->s_bdev;
	
	pr_err("U-MOUNT ENTRY %s\n", get_loop_backing_file(bd));
	return 0;
}

static int umount_handler(struct kretprobe_instance* krp_inst, struct pt_regs* regs) {
	pr_err("U-MOUNT EXIT\n");
	return 0;
}

int setup_epoch_mgmt(void) {
	krp_mount.entry_handler = mount_entry_handler;
	krp_mount.handler = mount_handler;
	krp_mount.kp.symbol_name = KRP_MOUNT_SYMBOL_NAME;
	krp_mount.maxactive = -1;

	krp_umount.entry_handler = umount_entry_handler;
	krp_umount.handler = umount_handler;
	krp_umount.kp.symbol_name = KRP_UMOUNT_SYMBOL_NAME;
	krp_umount.maxactive = -1;

	return register_kretprobes(krps, num_krps);
}

void destroy_epoch_mgmt(void) {
	unregister_kretprobes(krps, num_krps);
}

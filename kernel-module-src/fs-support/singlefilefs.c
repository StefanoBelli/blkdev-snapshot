#include <linux/kprobes.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/xxhash.h>

#include <bdsnap/bdsnap.h>
#include <fs-support/singlefilefs.h>

#define SINGLEFILEFS_MAGIC 0x42424242

static DEFINE_HASHTABLE(xkpblocks_ht, 18);
static DEFINE_SPINLOCK(xkpblocks_ht_lock);

struct xkpblocks_key {
	u64 tid;
	u64 tstart;
};

struct xkpblocks_node {
	struct xkpblocks_key key;

	char *block;
	u64 blocksize;
	u64 blocknum;

	struct hlist_node node;
};

static __always_inline u64 keyhashit(struct xkpblocks_key *key) {
	return xxh64(key, sizeof(struct xkpblocks_key), 0x123456789a9b9c9d);
}

static void remove_threadentry_now(pid_t tid, u64 tstart) {
	struct xkpblocks_key key = {
		.tid = (u64) tid,
		.tstart = tstart
	};

	struct xkpblocks_node *cur;
	struct hlist_node *tmp;

	unsigned long flags;
	spin_lock_irqsave(&xkpblocks_ht_lock, flags);

	hash_for_each_possible_safe(xkpblocks_ht, cur, tmp, node, keyhashit(&key)) {
		if(cur->key.tid == key.tid && cur->key.tstart == key.tstart) {
			if(cur->block != NULL) {
				kfree(cur->block);
			}

			hash_del(&cur->node);
			kfree(cur);
			break;
		}
	}

	spin_unlock_irqrestore(&xkpblocks_ht_lock, flags);
}

static struct xkpblocks_node* search_threadentry(pid_t tid, u64 tstart) {
	struct xkpblocks_key key = {
		.tid = (u64) tid,
		.tstart = tstart
	};

	struct xkpblocks_node *found = NULL;
	struct xkpblocks_node *cur;

	unsigned long flags;
	spin_lock_irqsave(&xkpblocks_ht_lock, flags);

	hash_for_each_possible(xkpblocks_ht, cur, node, keyhashit(&key)) {
		if(cur->key.tid == key.tid && cur->key.tstart == key.tstart) {
			found = cur;
			break;
		}
	}

	spin_unlock_irqrestore(&xkpblocks_ht_lock, flags);

	return found;
}

/**
 *
 * vfs_write
 *
 */

#define KRP_VFS_WRITE_SYMBOL_NAME "vfs_write"

static int vfs_write_entry_handler(
		__always_unused struct kretprobe_instance *krp_inst, 
		struct pt_regs* regs) {

	struct file *filp = (struct file*) regs->di;
	struct address_space *map;
	struct inode *hostino;
	struct super_block *sb;

	if(
			filp == NULL ||
			(map = filp->f_mapping) == NULL || 
			(hostino = map->host) == NULL || 
			(sb = hostino->i_sb) == NULL ||
			sb->s_magic != SINGLEFILEFS_MAGIC) {
		return 0;
	}

	struct xkpblocks_node *node = kmalloc(sizeof(struct xkpblocks_node), GFP_ATOMIC);
	if(node == NULL) {
		return 1;
	}

	INIT_HLIST_NODE(&node->node);
	node->key.tid = task_pid_nr(current);
	node->key.tstart = current->start_boottime;
	node->block = NULL;

	unsigned long flags;
	spin_lock_irqsave(&xkpblocks_ht_lock, flags);
	hash_add(xkpblocks_ht, &node->node, keyhashit(&node->key));
	spin_unlock_irqrestore(&xkpblocks_ht_lock, flags);

	return 0;
}

static int vfs_write_handler(
		__always_unused struct kretprobe_instance *krp_inst, 
		__always_unused struct pt_regs* regs) {

	remove_threadentry_now(task_pid_nr(current), current->start_boottime);
	return 0;
}

/**
 *
 * sb_bread
 *
 */

#define KRP_SB_BREAD_SYMBOL_NAME "__bread_gfp"

static int sb_bread_handler(
		__always_unused struct kretprobe_instance *krp_inst, 
		struct pt_regs* regs) {

	pid_t tid = task_pid_nr(current);
	u64 tstart = current->start_boottime;

	struct xkpblocks_node *threntry = search_threadentry(tid, tstart);
	if(threntry == NULL) {
		return 0;
	}

	struct buffer_head *bh = (struct buffer_head*) regs_return_value(regs);

	if((threntry->block = kmalloc(bh->b_size, GFP_ATOMIC)) == NULL) {
		remove_threadentry_now(tid, tstart);
		return 0;
	}

	memcpy(threntry->block, bh->b_data, bh->b_size);
	threntry->blocksize = bh->b_size;
	threntry->blocknum = bh->b_blocknr;

	return 0;
}

/**
 *
 * write_dirty_buffer
 *
 */

#define KP_WRITE_DIRTY_BUFFER_SYMBOL_NAME "write_dirty_buffer"

static int write_dirty_buffer_pre_handler(
		__always_unused struct kprobe *kp, 
		struct pt_regs *regs) {

	pid_t tid = task_pid_nr(current);
	u64 tstart = current->start_boottime;

	struct xkpblocks_node *threntry = search_threadentry(tid, tstart);
	if(threntry == NULL) {
		return 0;
	}

	struct buffer_head *bh = (struct buffer_head*) regs->di;
	if(bh->b_bdev == NULL) {
		remove_threadentry_now(tid, tstart);
		return 0;
	}

	unsigned long saved_cpu_flags;

	rcu_read_lock();

	void* handle = bdsnap_search_device(
			bh->b_bdev, &saved_cpu_flags);

	bdsnap_make_snapshot(
			handle, 
			threntry->block, 
			threntry->blocknum, 
			threntry->blocksize, 
			saved_cpu_flags);

	rcu_read_unlock();

	remove_threadentry_now(tid, tstart);
	return 0;
}


/**
 *
 * kretprobes
 *
 */

static struct kretprobe krp_vfs_write = {
	.kp.symbol_name = KRP_VFS_WRITE_SYMBOL_NAME,
	.entry_handler = vfs_write_entry_handler,
	.handler = vfs_write_handler,
	.maxactive = -1
};

static struct kretprobe krp_sb_bread = {
	.kp.symbol_name = KRP_SB_BREAD_SYMBOL_NAME,
	.handler = sb_bread_handler,
	.maxactive = -1
};

/**
 *
 * kprobes
 *
 */

static struct kprobe kp_write_dirty_buffer = {
	.symbol_name = KP_WRITE_DIRTY_BUFFER_SYMBOL_NAME,
	.pre_handler = write_dirty_buffer_pre_handler
};

/**
 *
 * which kretprobes to register
 *
 */

static struct kretprobe *krps_to_register[] = {
	&krp_vfs_write,
	&krp_sb_bread
};

static size_t num_krps_to_register = 
	sizeof(krps_to_register) / sizeof(struct kretprobe*);


/**
 *
 * which kprobes to register
 *
 */

static struct kprobe *kps_to_register[] = {
	&kp_write_dirty_buffer
};

static size_t num_kps_to_register = 
	sizeof(kps_to_register) / sizeof(struct kprobe*);

/**
 *
 * register/unregister fs-specific support
 *
 */

int register_fssupport_singlefilefs(void) {
	int krp_res = register_kretprobes(krps_to_register, num_krps_to_register);
	if(krp_res != 0) {
		return krp_res;
	}

	int kp_res = register_kprobes(kps_to_register, num_kps_to_register);
	if(kp_res != 0) {
		unregister_kretprobes(krps_to_register, num_krps_to_register);
		return kp_res;
	}

	return 0;
}

void unregister_fssupport_singlefilefs(void) {
	unregister_kretprobes(krps_to_register, num_krps_to_register);
	unregister_kprobes(kps_to_register, num_kps_to_register);
}

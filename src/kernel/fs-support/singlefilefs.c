#include <linux/kprobes.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/xxhash.h>

#include <bdsnap/bdsnap.h>
#include <fs-support/singlefilefs.h>
#include <pr-err-failure.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
#	define my_task_boottime(tsk) ((tsk)->start_boottime)
#else
#	define my_task_boottime(tsk) ((tsk)->real_start_time)
#endif

#define SINGLEFILEFS_MAGIC 0x42424242
#define SINGLEFILEFS_BLOCK_SIZE 4096

static DEFINE_HASHTABLE(xkpblocks_ht, 18);
static DEFINE_SPINLOCK(xkpblocks_ht_lock);

struct xkpblocks_key {
	u64 tid;
	u64 tstart;
};

#define DEFINE_XKPBLOCKS_KEY(_name, _tid, _tstart) \
	struct xkpblocks_key _name = { \
		.tid = ((u64) _tid), \
		.tstart = (_tstart) \
	}

struct xkpblocks_node {
	struct xkpblocks_key key;

	char block[SINGLEFILEFS_BLOCK_SIZE];
	u64 blocknum;

	struct hlist_node node;
	struct rcu_head rcu;
};

static __always_inline u64 keyhashit(struct xkpblocks_key *key) {
	return xxh64(key, sizeof(struct xkpblocks_key), 0x123456789a9b9c9d);
}

static void xkpblocks_rcu_free_fn(struct rcu_head *rcu) {
	struct xkpblocks_node *n = container_of(rcu, struct xkpblocks_node, rcu);
	free_pages_exact(n, sizeof(struct xkpblocks_node));
}

static inline struct xkpblocks_node* search_threadentry(pid_t tid, u64 tstart) {
	DEFINE_XKPBLOCKS_KEY(key, tid, tstart);

	struct xkpblocks_node *found = NULL;
	struct xkpblocks_node *cur;

	hash_for_each_possible_rcu(xkpblocks_ht, cur, node, keyhashit(&key)) {
		if(cur->key.tid == key.tid && cur->key.tstart == key.tstart) {
			found = cur;
			break;
		}
	}

	return found;
}

static inline void remove_threadentry(struct xkpblocks_node *thrent) {
	unsigned long cpu_flags;
	spin_lock_irqsave(&xkpblocks_ht_lock, cpu_flags);
	hash_del_rcu(&thrent->node);
	spin_unlock_irqrestore(&xkpblocks_ht_lock, cpu_flags);

	call_rcu(&thrent->rcu, xkpblocks_rcu_free_fn);
}

static void cleanup_xkpblocks_ht(void) {
	size_t bktnr;
	struct xkpblocks_node *cur;
	struct hlist_node *tmp;

	spin_lock(&xkpblocks_ht_lock);

	hash_for_each_safe(xkpblocks_ht, bktnr, tmp, cur, node) {
		hash_del_rcu(&cur->node);
		call_rcu(&cur->rcu, xkpblocks_rcu_free_fn);
	}

	spin_unlock(&xkpblocks_ht_lock);
}

static bool __do_taskcheck_on(u64 key_tid, u64 key_tstart) {
	bool res = false;

	struct task_struct *p;

	rcu_read_lock();

	struct pid *pid = find_pid_ns(key_tid, &init_pid_ns);
	p = pid_task(pid, PIDTYPE_PID);

	if(p == NULL) {
		goto ____do_taskcheck_on_finish0;
	}

	get_task_struct(p);
	u64 tstart = my_task_boottime(p);
	int texited = p->exit_state;
	
	if(tstart != key_tstart || texited) {
		goto ____do_taskcheck_on_finish1;
	}

	res = true;

____do_taskcheck_on_finish1:
	put_task_struct(p);

____do_taskcheck_on_finish0:
	rcu_read_unlock();

	return res;
}

static struct delayed_work gdwork_taskcheck;

#define schedule_taskcheck_work(_dwork) \
	do { \
		if(!schedule_delayed_work((_dwork), msecs_to_jiffies(3600000))) { \
			pr_err_failure("schedule_delayed_work"); \
		} \
	} while(0)

static void periodic_taskcheck_xkpblocks_ht(struct work_struct *work) {
	static u32 next_bktnr = 0;
	u32 end = min(next_bktnr + 50, HASH_SIZE(xkpblocks_ht));

	spin_lock(&xkpblocks_ht_lock);

	for (u32 bktnr = next_bktnr; bktnr < end; bktnr++) {
		struct xkpblocks_node *cur;
        struct hlist_node *tmp;

        hlist_for_each_entry_safe(cur, tmp, &xkpblocks_ht[bktnr], node) {
            if (!__do_taskcheck_on(cur->key.tid, cur->key.tstart)) {
            	hash_del_rcu(&cur->node);
            	call_rcu(&cur->rcu, xkpblocks_rcu_free_fn);
            }
        }
    }

    spin_unlock(&xkpblocks_ht_lock);

    if(end < HASH_SIZE(xkpblocks_ht)) {
    	next_bktnr += end;
    } else {
    	next_bktnr = 0;
    }

    schedule_taskcheck_work(to_delayed_work(work));
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
	struct xkpblocks_node *node;

	if(
			filp == NULL ||
			(map = filp->f_mapping) == NULL || 
			(hostino = map->host) == NULL || 
			(sb = hostino->i_sb) == NULL ||
			sb->s_magic != SINGLEFILEFS_MAGIC ||
			sb->s_bdev == NULL ||
			!bdsnap_test_device(sb->s_bdev) || 
			(node = alloc_pages_exact(sizeof(struct xkpblocks_node), GFP_ATOMIC)) == NULL) {

		return 1;
	}

	INIT_HLIST_NODE(&node->node);
	node->key.tid = task_pid_nr(current);
	node->key.tstart = my_task_boottime(current);

	unsigned long flags;
	spin_lock_irqsave(&xkpblocks_ht_lock, flags);
	hash_add_rcu(xkpblocks_ht, &node->node, keyhashit(&node->key));
	spin_unlock_irqrestore(&xkpblocks_ht_lock, flags);

	return 0;
}

static int vfs_write_handler(
		__always_unused struct kretprobe_instance *krp_inst, 
		__always_unused struct pt_regs* regs) {

	pid_t tid = task_pid_nr(current);
	u64 tstart = my_task_boottime(current);

	rcu_read_lock();

	struct xkpblocks_node *cur = search_threadentry(tid, tstart);
	if(cur == NULL) {
		rcu_read_unlock();
		return 0;
	}

	rcu_read_unlock();
	remove_threadentry(cur);

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
	u64 tstart = my_task_boottime(current);

	rcu_read_lock();

	struct xkpblocks_node *threntry = search_threadentry(tid, tstart);
	if(threntry == NULL) {
		rcu_read_unlock();
		return 0;
	}

	struct buffer_head *bh = (struct buffer_head*) regs_return_value(regs);

	memcpy(threntry->block, bh->b_data, bh->b_size);
	threntry->blocknum = bh->b_blocknr;

	rcu_read_unlock();

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
	u64 tstart = my_task_boottime(current);

	rcu_read_lock();

	struct xkpblocks_node *threntry = search_threadentry(tid, tstart);
	if(threntry == NULL) {
		rcu_read_unlock();
		return 0;
	}

	struct buffer_head *bh = (struct buffer_head*) regs->di;
	if(bh->b_bdev == NULL) {
		rcu_read_unlock();
		remove_threadentry(threntry);
		return 0;
	}

	if(bh->b_size != SINGLEFILEFS_BLOCK_SIZE) {
		rcu_read_unlock();
		remove_threadentry(threntry);
		BUG();
		return 0; //unreachable code
	}

	unsigned long saved_cpu_flags;

	void* handle = bdsnap_search_device(
			bh->b_bdev, &saved_cpu_flags);

	bdsnap_make_snapshot(
			handle, 
			threntry->block, 
			bh->b_blocknr, 
			SINGLEFILEFS_BLOCK_SIZE, 
			saved_cpu_flags);

	rcu_read_unlock();
	remove_threadentry(threntry);
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

static const size_t num_krps_to_register = 
	sizeof(krps_to_register) / sizeof(struct kretprobe*);


/**
 *
 * which kprobes to register
 *
 */

static struct kprobe *kps_to_register[] = {
	&kp_write_dirty_buffer
};

static const size_t num_kps_to_register = 
	sizeof(kps_to_register) / sizeof(struct kprobe*);

/**
 *
 * register/unregister fs-specific support
 *
 */

int register_fssupport_singlefilefs(void) {
	int krp_res = register_kretprobes(krps_to_register, num_krps_to_register);
	if(krp_res != 0) {
		pr_err_failure_with_code("register_kretprobes", krp_res);
		return krp_res;
	}

	int kp_res = register_kprobes(kps_to_register, num_kps_to_register);
	if(kp_res != 0) {
		pr_err_failure_with_code("register_kprobes", kp_res);
		unregister_kretprobes(krps_to_register, num_krps_to_register);
		return kp_res;
	}

	INIT_DELAYED_WORK(&gdwork_taskcheck, periodic_taskcheck_xkpblocks_ht);
	schedule_taskcheck_work(&gdwork_taskcheck);

	return 0;
}

void unregister_fssupport_singlefilefs(void) {
	unregister_kretprobes(krps_to_register, num_krps_to_register);
	unregister_kprobes(kps_to_register, num_kps_to_register);

	if(!cancel_delayed_work_sync(&gdwork_taskcheck)) {
		pr_err_failure("cancel_delayed_work_sync");
	}

	cleanup_xkpblocks_ht();
}

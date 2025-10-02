#include <linux/slab.h>
#include <linux/version.h>

#include <lru.h>
#include <pr-err-failure.h>

typedef bool (*llru_op_fpt)(struct list_lru*, struct list_head*);

//this is because of introduction of NUMA-aware list_lru in v6.8.0
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
llru_op_fpt llru_add = list_lru_add_obj;
llru_op_fpt llru_del = list_lru_del_obj;
#elif KERNEL_VERSION(3,12,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0)
llru_op_fpt llru_add = list_lru_add;
llru_op_fpt llru_del = list_lru_del;
#else
#	error unsupported kernel version (missing list_lru_add, list_lru_del)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,13,0)

#	define LIST_LRU_WALK_CB_ARGS \
		struct list_head *item, \
		__always_unused struct list_lru_one *l, \
		__maybe_unused void *args

#elif KERNEL_VERSION(4,0,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)

#	define LIST_LRU_WALK_CB_ARGS \
		struct list_head *item, \
		__always_unused struct list_lru_one *l, \
		__always_unused spinlock_t *s, \
		__maybe_unused void *args

#elif KERNEL_VERSION(3,12,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)

#	define LIST_LRU_WALK_CB_ARGS \
		struct list_head *item, \
		__always_unused spinlock_t *s, \
		__maybe_unused void *args

#else
#	error unsupported kernel version (missing typedef list_lru_walk_cb)
#endif

struct lru_node {
	struct list_head lru;
	sector_t blknr;
};

static enum lru_status evict_cb(LIST_LRU_WALK_CB_ARGS) {
	struct lru_node *node = 
		container_of(item, struct lru_node, lru);

	list_del_init(&node->lru);
	kfree(node);

	return LRU_REMOVED;
}

static void enforce_lru_size_limit(struct list_lru * lru) {
	unsigned long cnt = list_lru_count(lru);
	if (cnt <= NUM_MAX_LRU_ENTRIES)
		return;

	//this should always be 1
	unsigned long over = cnt - NUM_MAX_LRU_ENTRIES;

	list_lru_walk(lru, evict_cb, NULL, over);
}

struct lookup_args {
	sector_t key;
	struct lru_node *hit_node;
};

static enum lru_status find_and_stop_cb(LIST_LRU_WALK_CB_ARGS) {
	struct lookup_args *lkargs = args;
	struct lru_node *node = 
		container_of(item, struct lru_node, lru);

	if (node->blknr == lkargs->key) {
		lkargs->hit_node = node;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0)
		return LRU_STOP;
#endif
	}

	return LRU_SKIP;
}

bool lookup_lru(struct list_lru * lru, sector_t blknr) {
	struct lookup_args lkargs = { 
		.key = blknr,
		.hit_node = NULL
	};

	list_lru_walk(lru, find_and_stop_cb, &lkargs, ULONG_MAX);

	//node becomes the most recently used object
	if (lkargs.hit_node != NULL) {
		if(!llru_del(lru, &lkargs.hit_node->lru)) {
			pr_err_failure("llru_del");
			return false;
		}

		if(!llru_add(lru, &lkargs.hit_node->lru)) {
			pr_err_failure("llru_add");
			return false;
		}

		return true;
	}

	return false;
}

bool add_lru(struct list_lru * lru, sector_t blknr) {
	struct lru_node *node = 
		kmalloc(sizeof(struct lru_node), GFP_KERNEL);
	if (node == NULL) {
		pr_err_failure("kmalloc");
		return false;
	}

	INIT_LIST_HEAD(&node->lru);
	node->blknr = blknr;

	if(!llru_add(lru, &node->lru)) {
		pr_err_failure("llru_add");
		kfree(node);
		return false;
	}

	enforce_lru_size_limit(lru);

	return true;
}

void destroy_all_elems_in_lru(struct list_lru *lru) {
	list_lru_walk(lru, evict_cb, NULL, ULONG_MAX);
}


#include <linux/version.h>
#include <linux/slab.h>

#include <lru-ng.h>
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
	struct hlist_node hnode;
	struct list_head lru;
	sector_t blknr;
};

bool lru_ng_init(struct lru_ng* lru) {
	hash_init(lru->hasht);
	return list_lru_init(&lru->llru) == 0;
}

static enum lru_status evict_cb(LIST_LRU_WALK_CB_ARGS) {
	struct lru_node *node = 
		container_of(item, struct lru_node, lru);

	list_del_init(&node->lru);
	hash_del(&node->hnode);
	kfree(node);

	return LRU_REMOVED;
}

static inline void enforce_lru_size_limit(struct list_lru * lru) {
	if (list_lru_count(lru) <= LRU_NG__LRU_MAX_ENTRIES) {
		return;
	}

	list_lru_walk(lru, evict_cb, NULL, 1);
}

bool lru_ng_add(struct lru_ng * lru, sector_t key) {
	struct lru_node *node = kzalloc(sizeof(struct lru_node), GFP_KERNEL);
	if(node == NULL) {
		pr_err_failure("kmalloc");
		return false;
	}

	hash_add(lru->hasht, &node->hnode, key);

	INIT_LIST_HEAD(&node->lru);
	node->blknr = key;

	if(!llru_add(&lru->llru, &node->lru)) {
		pr_err_failure("llru_add");
		hash_del(&node->hnode);
		kfree(node);
		return false;
	}
	
	enforce_lru_size_limit(&lru->llru);

	return true;
}

bool lru_ng_lookup(struct lru_ng * lru, sector_t key) {
	struct lru_node *node;

	hash_for_each_possible(lru->hasht, node, hnode, key) {
		if(node->blknr == key) {
			if(!llru_del(&lru->llru, &node->lru)) {
				pr_err_failure("llru_del");
				return false;
			}

			if(!llru_add(&lru->llru, &node->lru)) {
				pr_err_failure("llru_add");
				return false;
			}

			return true;
		}
	}

	return false;
}

void lru_ng_cleanup(struct lru_ng* lru) {
	list_lru_walk(&lru->llru, evict_cb, NULL, ULONG_MAX);
	list_lru_destroy(&lru->llru);
}

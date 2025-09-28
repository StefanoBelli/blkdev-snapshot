#include <linux/slab.h>

#include <lru.h>

struct lru_node {
	struct list_head lru;
	sector_t blknr;
};

static enum lru_status evict_cb(
		struct list_head *item, 
		struct list_lru_one *list, 
		void *args) {

    //struct list_head *freeable = arg;

    struct lru_node *node = 
    	container_of(item, struct lru_node, lru);

    list_del_init(&node->lru);
    kfree(node);

    //list_add(&node->lru, freeable);

    return LRU_REMOVED;
}

static void enforce_lru_size_limit(struct list_lru * lru) {
	unsigned long cnt = list_lru_count(lru);
    if (cnt <= NUM_MAX_LRU_ENTRIES)
        return;

    unsigned long over = cnt - NUM_MAX_LRU_ENTRIES;
    //LIST_HEAD(freeable);

    list_lru_walk(lru, evict_cb, NULL /*&freeable*/, over);

    /*
    while (!list_empty(&freeable)) {
        struct lru_node *node = 
        	list_first_entry(&freeable, struct lru_node, lru);
        list_del_init(&node->lru);
        kfree(node);
    }*/
}

struct lookup_args {
	sector_t key;
	struct lru_node *hit_node;
};

static enum lru_status find_and_stop_cb(
		struct list_head *item,
		struct list_lru_one *list,
		void *args) {

    struct lookup_args *lkargs = args;
    struct lru_node *node = 
    	container_of(item, struct lru_node, lru);

    if (node->blknr == lkargs->key) {
        lkargs->hit_node = node;
        return LRU_STOP;
    }

    return LRU_SKIP;
}

bool lookup_lru(struct list_lru * lru, sector_t blknr) {
	struct lookup_args lkargs = { 
		.key = blknr,
		.hit_node = NULL
	};

    list_lru_walk(lru, find_and_stop_cb, &lkargs, ULONG_MAX);

    if (lkargs.hit_node != NULL) {
        list_lru_del_obj(lru, &lkargs.hit_node->lru);
        list_lru_add_obj(lru, &lkargs.hit_node->lru);

        return true;
    }

    return false;
}

bool add_lru(struct list_lru * lru, sector_t blknr) {
	struct lru_node *node = 
		kmalloc(sizeof(struct lru_node), GFP_KERNEL);
    if (node == NULL) {
        return false;
    }

    INIT_LIST_HEAD(&node->lru);
    node->blknr = blknr;

    if(!list_lru_add_obj(lru, &node->lru)) {
    	kfree(node);
    	return false;
    }

    enforce_lru_size_limit(lru);

    return true;
}

void destroy_all_elems_in_lru(struct list_lru *lru) {
	//LIST_HEAD(elems);
	list_lru_walk(lru, evict_cb, NULL, ULONG_MAX);

	/*while (!list_empty(&elems)) {
        struct lru_node *node = 
        	list_first_entry(&elems, struct lru_node, lru);
        list_del_init(&node->lru);
        kfree(node);
    }*/
}


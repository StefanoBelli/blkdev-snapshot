#ifndef LRU_NG_H
#define LRU_NG_H

#include <linux/list_lru.h>
#include <linux/hashtable.h>

#define LRU_NG__HT_BUCKET_BITS 16
#define LRU_NG__LRU_MAX_ENTRIES (1 << LRU_NG__HT_BUCKET_BITS)

struct lru_ng {
	struct list_lru llru;
	DECLARE_HASHTABLE(hasht, LRU_NG__HT_BUCKET_BITS);
};

bool lru_ng_init(struct lru_ng* lru);
bool lru_ng_lookup(struct lru_ng * lru, sector_t key);
bool lru_ng_add(struct lru_ng * lru, sector_t key);
void lru_ng_cleanup(struct lru_ng* lru);

#endif


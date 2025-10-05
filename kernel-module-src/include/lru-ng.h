#ifndef LRU_NG_H
#define LRU_NG_H

#include <linux/list_lru.h>
#include <linux/hashtable.h>

#define LRU_NG__HT_BUCKET_BITS 16
#define LRU_NG__LRU_MAX_ENTRIES (1 << LRU_NG__HT_BUCKET_BITS)

struct lru_ng; //opaque ptr

struct lru_ng* lru_ng_alloc_and_init(void);
bool lru_ng_lookup(struct lru_ng * lru, sector_t key);
bool lru_ng_add(struct lru_ng * lru, sector_t key);
void lru_ng_cleanup_and_destroy(struct lru_ng* lru);

#endif


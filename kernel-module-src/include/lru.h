#ifndef LRU_H
#define LRU_H

#include <linux/list_lru.h>

#define NUM_MAX_LRU_ENTRIES 8192

bool lookup_lru(struct list_lru *, sector_t);
bool add_lru(struct list_lru *, sector_t);
void destroy_all_elems_in_lru(struct list_lru *);

#endif

#ifndef EPOCH_H
#define EPOCH_H

#include <linux/list_lru.h>
#include <linux/dcache.h>

#include <mounts.h>

struct epoch {
	int n_currently_mounted;
	struct dentry *d_snapdir;
	struct list_lru *cached_blocks;
};

#define epoch_destroy_cached_blocks_lru(e) do { \
		if((e).cached_blocks != NULL) { \
			list_lru_destroy((e).cached_blocks); \
			kfree((e).cached_blocks); \
			(e).cached_blocks = NULL; \
		} \
	} while(0)

#define epoch_destroy_d_snapdir_dentry(e) do { \
		if((e).d_snapdir != NULL) { \
			dput((e).d_snapdir); \
			(e).d_snapdir = NULL; \
		} \
	} while(0)

void epoch_count_mount(const struct mountinfo*);
void epoch_count_umount(const struct mountinfo*);

#endif

#ifndef GET_BLOCK_MAGIC_H
#define GET_BLOCK_MAGIC_H

#include <linux/version.h>
#include <linux/blk_types.h>
#include <linux/fs.h>

static inline unsigned long get_block_magic(const struct block_device* b) {
	// https://www.linuxquestions.org/questions/showthread.php?p=6490737#post6490737
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)
	struct super_block *sb = (struct super_block*) b->bd_holder;
#else
	struct super_block *sb = b->bd_super;
#endif

	return sb->s_magic;
}

#endif
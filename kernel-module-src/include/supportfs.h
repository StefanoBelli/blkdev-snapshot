#ifndef SUPPORT_FS_H
#define SUPPORT_FS_H

#include <linux/module.h>

struct bdsnap_supported_fs {
	unsigned long magic;
	int (*init_support)(void);
	void (*finish_support)(void);
	struct module *owner;
};

/**
 * Each of the following functions must be called
 * within process context
 */

/* exported symbols (gpl) */

#define BDSNAP_ERR_REG_OK 0
#define BDSNAP_ERR_REG_MTXLCKEINTR 1
#define BDSNAP_ERR_REG_CANNOTREG 2
#define BDSNAP_ERR_REG_ALREADYSUPP 3
#define BDSNAP_ERR_REG_MEMEXHAUSTED 4
#define BDSNAP_ERR_REG_INITFAIL 5
#define BDSNAP_ERR_REG_MODGET 6

int bdsnap_register_supported_fs(const struct bdsnap_supported_fs*);

/* non-exported symbols (only avail within this module) */

#define BDSNAP_ERR_CLEANUP_OK 0
#define BDSNAP_ERR_CLEANUP_MTXLCKEINTR 1
#define BDSNAP_ERR_CLEANUP_ALREADYDONE 2

int bdsnap_cleanup_supported_fs(void);

bool bdsnap_has_supported_fs(unsigned long);

#endif
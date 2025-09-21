#ifndef KMALLOC_FAILED_H
#define KMALLOC_FAILED_H

#include <linux/printk.h>
#include <linux/module.h>

#define print_kmalloc_failed() do { \
		pr_err("%s: kmalloc() failed (message " \
			"issued at file: %s, " \
			"line: %d)", \
			module_name(THIS_MODULE), __FILE__, __LINE__); \
	} while(0)

#endif
#ifndef PR_ERR_FAILURE_H
#define PR_ERR_FAILURE_H

#include <linux/printk.h>
#include <linux/module.h>

#define __pr_err_failure_base__(sym, codemsgfmt, code) do { \
		pr_err( \
			"%s: %s(...) failed " codemsgfmt "(message " \
			"issued at " \
			"file: %s, "\
			"func: %s, "\
			"line: %d)\n", \
			module_name(THIS_MODULE), \
			sym, \
			code, \
			__FILE__, \
			__func__, \
			__LINE__ \
		); \
	} while(0)

#define pr_err_failure(sym) \
	__pr_err_failure_base__(sym, "with error=%s", "[no code] ")

#define pr_err_failure_with_code(sym, code) \
	__pr_err_failure_base__(sym, "with error=%ld ", ((long int) code))

#endif
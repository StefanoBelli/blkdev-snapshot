#ifndef FS_SUPPORT_H
#define FS_SUPPORT_H

#include <linux/module.h>

#include <fs-support/singlefilefs.h>

struct fssupport_struct {
	const char* name;
	int (*regi)(void);
	void (*unregi)(void);
};

static struct fssupport_struct supported_fs[] = {
	{ 
		"singlefilefs", 
		register_fssupport_singlefilefs, 
		unregister_fssupport_singlefilefs 
	}
};

static const size_t num_supp_fs = 
	sizeof(supported_fs) / sizeof(struct fssupport_struct);

static int setup_fssupport(void) {
	for(size_t i = 0; i < num_supp_fs; i++) {
		int err = supported_fs[i].regi();
		if(err != 0) {
			pr_err("%s: unable to register filesystem named \"%s\" (idx = %ld).\n"
					"Its regifn failed with code: %d\n", 
					module_name(THIS_MODULE), supported_fs[i].name, i, err);
			for(size_t j = 0; j < i; j++) {
				supported_fs[i].unregi();
			}

			return 1;
		}
	}

	return 0;
}

static void destroy_fssupport(void) {
	for(size_t i = 0; i < num_supp_fs; i++) {
		supported_fs[i].unregi();
	}
}

#endif

#ifndef FS_SUPPORT_H
#define FS_SUPPORT_H

#include <linux/module.h>

int register_fssupport_singlefilefs(void);
void unregister_fssupport_singlefilefs(void);

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

static inline int setup_fssupport(void) {
	int num_supp_fs = sizeof(supported_fs) / sizeof(struct fssupport_struct);
	for(int i = 0; i < num_supp_fs; i++) {
		if(supported_fs[i].regi() != 0) {
			pr_err("%s: unable to register filesystem named \"%s\"\n", 
					module_name(THIS_MODULE), supported_fs[i].name);
			for(int j = 0; j < i; j++) {
				supported_fs[i].unregi();
			}

			return 1;
		}
	}

	return 0;
}

static inline void destroy_fssupport(void) {
	int num_supp_fs = sizeof(supported_fs) / sizeof(struct fssupport_struct);
	for(int i = 0; i < num_supp_fs; i++) {
		supported_fs[i].unregi();
	}
}

#endif

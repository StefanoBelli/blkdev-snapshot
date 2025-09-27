#include <activation.h>
#include <devices.h>
#include <mounts.h>
#include <pr-err-failure.h>


#ifdef CONFIG_SYSFS
char* activation_ct_passwd;
module_param_named(actpasswd, activation_ct_passwd, charp, 0);
#else
#ifndef ACTDEVREQMAJ
#warning you may want to request a particular major number (define ACTDEVREQMAJ)
#define ACTDEVREQMAJ 0
#endif

#ifndef ACTPASSWD
#error you must set a password (define ACTPASSWD)
#endif

#warning password is cleartext-hardcoded in ko! \
	You may want to find a way to secure your .ko file \
	such that it is not possible to do any kind of analysis \
	on it (e.g. objdump)

const char* activation_ct_passwd = ACTPASSWD;
unsigned int activation_dev_req_maj = ACTDEVREQMAJ;
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Block device snapshot");
MODULE_AUTHOR("Stefano Belli");

int __init init_blkdev_snapshot_module(void);
void __exit exit_blkdev_snapshot_module(void);

#define __SETUP_RVVAR rv
#define START_SETUP_BLOCK int __SETUP_RVVAR = 0
#define END_SETUP_BLOCK return __SETUP_RVVAR

#define _SETUP(name) \
	__SETUP_RVVAR = setup_##name(); \
	if(__SETUP_RVVAR != 0)

#define pr_err_setup(name) \
	pr_err_failure_with_code("setup"#name, __SETUP_RVVAR)

int __init init_blkdev_snapshot_module(void) {
	if(activation_ct_passwd == NULL || strlen(activation_ct_passwd) == 0) {
		pr_err("%s: no password was provided\n", module_name(THIS_MODULE));
		return -ENODATA;
	}

	START_SETUP_BLOCK;

	_SETUP(devices) {
		pr_err_setup(devices);
		END_SETUP_BLOCK;
	}

	_SETUP(mounts) {
		pr_err_setup(epoch_mgmt);
		destroy_devices();
		END_SETUP_BLOCK;
	}

	_SETUP(activation_mechanism) {
		pr_err_setup(activation_mechanism);
		destroy_mounts();
		destroy_devices();
		END_SETUP_BLOCK;
	}

	END_SETUP_BLOCK;
}

#undef __SETUP_RVVAR
#undef START_SETUP_BLOCK
#undef END_SETUP_BLOCK
#undef _SETUP
#undef pr_err_setup

void __exit exit_blkdev_snapshot_module(void) {
	destroy_activation_mechanism();
	destroy_mounts();
	destroy_devices();
}

module_init(init_blkdev_snapshot_module);
module_exit(exit_blkdev_snapshot_module);

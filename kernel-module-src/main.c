#include <linux/kernel.h>
#include <linux/module.h>

#include <activation.h>

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

#warning password is cleartext-hardcoded in ko, \
cannot salt-hash in memory! \
you may want to find a way to secure your .ko file \
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


int __init init_blkdev_snapshot_module(void) {
    if(activation_ct_passwd == NULL || strlen(activation_ct_passwd) == 0) {
        pr_err("%s: no password was provided\n", module_name(THIS_MODULE));
        return -ENODATA;
    }

    return setup_activation_mechanism();
}

void __exit exit_blkdev_snapshot_module(void) {
    kfree(activation_ct_passwd);
    destroy_activation_mechanism();
}

module_init(init_blkdev_snapshot_module);
module_exit(exit_blkdev_snapshot_module);

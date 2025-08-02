#include <linux/kernel.h>
#include <linux/module.h>
#include <activation.h>

char* activation_ct_passwd = NULL;
module_param_named(actpasswd, activation_ct_passwd, charp, 0);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Block device snapshot");
MODULE_AUTHOR("Stefano Belli");

int __init init_blkdev_snapshot_module(void);
void __exit exit_blkdev_snapshot_module(void);


int __init init_blkdev_snapshot_module(void) {
    if(activation_ct_passwd == NULL || strlen(activation_ct_passwd) == 0) {
        pr_err("no password was provided\n");
        return -ENODATA;
    }

    return setup_activation_mechanism_via_sysfs();
}

void __exit exit_blkdev_snapshot_module(void) {
    destroy_activation_mechanism_via_sysfs();
}

module_init(init_blkdev_snapshot_module);
module_exit(exit_blkdev_snapshot_module);

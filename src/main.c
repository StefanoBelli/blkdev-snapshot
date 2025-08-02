#include <linux/kernel.h>
#include <linux/module.h>
#include <activation.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Block device snapshot");
MODULE_AUTHOR("Stefano Belli");

int __init init_blkdev_snapshot_module(void);
void __exit exit_blkdev_snapshot_module(void);


int __init init_blkdev_snapshot_module(void) {
    return allow_activation_mechanism_via_sysfs();
}

void __exit exit_blkdev_snapshot_module(void) {
    destroy_activation_mechanism_via_sysfs();
}

module_init(init_blkdev_snapshot_module);
module_exit(exit_blkdev_snapshot_module);

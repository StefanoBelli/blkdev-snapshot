#include <linux/rhashtable.h>
#include <linux/namei.h>

#include <devices.h>

struct blkdev_object {
	dev_t key;
	unsigned long magic;

	struct hlist_head linkage;
};

const static struct rhashtable_params blkdevs_ht_params = {
	.key_len = sizeof(dev_t),
	.key_offset = offsetof(struct blkdev_object, key),
	.head_offset = offsetof(struct blkdev_object, linkage)
};

static struct rhashtable blkdevs_ht;

struct loop_object {
	char key[PATH_MAX];
	unsigned long magic;

	struct hlist_head linkage;
};

const static struct rhashtable_params loops_ht_params = {
	.key_len = sizeof(char) * PATH_MAX,
	.key_offset = offsetof(struct loop_object, key),
	.head_offset = offsetof(struct loop_object, linkage)
};

static struct rhashtable loops_ht;


static int get_inode_from_path(const char *path_str, struct inode **out_inode) {
	struct path path;
	int err = kern_path(path_str, LOOKUP_FOLLOW, &path);
	if (err) {
		return err;
	}

	*out_inode = path.dentry->d_inode;

	path_put(&path);

	return 0;
}

int register_device(const char* path) {
	struct inode *ino;
	int err = get_inode_from_path(path, &ino);
	if(err) {
		return err;
	}

	if(ino->i_mode & S_IFBLK) {

	} else if(ino->i_mode & S_IFREG) {

	} else {
		return -EINVAL;
	}

	//check if magic is compat with supported ones...

	return 0;
}

bool unregister_device(const char* path) {

}

bool check_device(const struct block_device* bdev, unsigned long *out_magic) {
		
}

EXPORT_SYMBOL_GPL(check_device);

bool setup_devices(void) {
	if(rhashtable_init(&blkdevs_ht, &blkdevs_ht_params) != 0) {
		return false;
	}

	if(rhashtable_init(&loops_ht, &loops_ht_params) != 0) {
		return false;
	}

	return true;
}

bool destroy_devices(void) {

}
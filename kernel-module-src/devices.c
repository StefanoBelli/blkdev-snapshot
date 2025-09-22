#include <linux/rhashtable.h>
#include <linux/namei.h>
#include <linux/blkdev.h>
#include <linux/major.h>
#include <linux/loop.h>
#include <linux/rcupdate.h>
#include <linux/version.h>

#include <devices.h>
#include <supportfs.h>
#include <kmalloc-failed.h>

struct blkdev_object {
	struct rhash_head linkage;
	struct rcu_head rcu;

	dev_t key;
	unsigned long magic;
};

static const struct rhashtable_params blkdevs_ht_params = {
	.key_len = sizeof(dev_t),
	.key_offset = offsetof(struct blkdev_object, key),
	.head_offset = offsetof(struct blkdev_object, linkage)
};

static struct rhashtable blkdevs_ht;

static void blkdevs_ht_free_fn(void* ptr, void* arg) {
	struct blkdev_object *bdptr = (struct blkdev_object*) ptr;
	kfree_rcu(bdptr, rcu);
}

struct loop_object {
	struct rhash_head linkage;
	struct rcu_head rcu;

	char key[PATH_MAX];
	unsigned long magic;
};

static const struct rhashtable_params loops_ht_params = {
	.key_len = sizeof(char) * PATH_MAX,
	.key_offset = offsetof(struct loop_object, key),
	.head_offset = offsetof(struct loop_object, linkage)
};

static struct rhashtable loops_ht;

static void loops_ht_free_fn(void* ptr, void* arg) {
	struct loop_object *loptr = (struct loop_object*) ptr;
	kfree_rcu(loptr, rcu);
}


static int get_full_path(const char* path, char* out_full_path) {
	struct path _path;
	int err = kern_path(path, LOOKUP_FOLLOW, &_path);
	if (err) {
		return err;
	}

	void* _ptr_in_buf = d_path(&_path, out_full_path, sizeof(char) * PATH_MAX);
	if(IS_ERR(_ptr_in_buf)) {
		pr_err("%s: d_path(...) failed with error=%ld\n", 
			module_name(THIS_MODULE), PTR_ERR(_ptr_in_buf));
		path_put(&_path);
		return PTR_ERR(_ptr_in_buf);
	}

	path_put(&_path);

	return 0;
}

static int get_inode_from_path(const char *path, struct inode **out_inode) {
	struct path _path;
	int err = kern_path(path, LOOKUP_FOLLOW, &_path);
	if (err) {
		return err;
	}

	*out_inode = _path.dentry->d_inode;

	path_put(&_path);

	return 0;
}

static char *get_loop_device_backing_file(const struct block_device *bdev) {
	char *out_backing_path = (char*) kmalloc(sizeof(char) * PATH_MAX, GFP_KERNEL);
	if(out_backing_path == NULL) {
		print_kmalloc_failed();
		return NULL;
	}

	const char *disk_name = bdev->bd_disk->disk_name;
	char *sysfs_path = (char*) kmalloc(sizeof(char) * PATH_MAX, GFP_KERNEL);
	if(sysfs_path == NULL) {
		kfree(out_backing_path);
		print_kmalloc_failed();
		return NULL;
	}

	snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/loop/backing_file", disk_name);

	struct file *filp = filp_open(sysfs_path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		kfree(sysfs_path);
		kfree(out_backing_path);	
		pr_err("%s: filp_open(...) failed with error=%ld\n", 
			module_name(THIS_MODULE), PTR_ERR(filp));
		return NULL;
	}

	kfree(sysfs_path);

	ssize_t read_err = kernel_read(filp, out_backing_path, PATH_MAX - 1, NULL);
	if (read_err <= 0) {
		kfree(out_backing_path);
		filp_close(filp, 0);
		pr_err("%s: kernel_read(...) failed with error=%ld\n", 
			module_name(THIS_MODULE), read_err);
		return NULL;
	}

	out_backing_path[read_err] = 0;

	filp_close(filp, 0);
	return out_backing_path;
}

static bool get_loop_magic(const char* p, unsigned long *out_magic) {
	struct file *filp = filp_open(p, O_RDONLY, 0);
	if(IS_ERR(filp)) {
		pr_err("%s: filp_open(...) failed with error=%ld\n", 
			module_name(THIS_MODULE), PTR_ERR(filp));
		return false;
	}

	ssize_t read_err = kernel_read(filp, out_magic, sizeof(*out_magic), NULL);
	if(read_err != sizeof(*out_magic)) {
		filp_close(filp, 0);
		pr_err("%s: kernel_read(...) did not respond as expected [read_err = %ld] != [expected = %ld]\n",
			module_name(THIS_MODULE), read_err, sizeof(*out_magic));
		return false;
	}

	filp_close(filp, 0);
	return true;
}

static int try_to_insert_loop_device(const char* path) {
	unsigned long magic;
	if(!get_loop_magic(path, &magic)) {
		return -EFAULT;
	}

	if(!bdsnap_has_supported_fs(magic)) {
		return -EBADTYPE;
	}

	struct loop_object *new_obj = kzalloc(sizeof(struct loop_object), GFP_KERNEL);
	if(new_obj == NULL) {
		print_kmalloc_failed();
		return -ENOMEM;
	}

	new_obj->magic = magic;

	int gfpath_err = get_full_path(path, new_obj->key);
	if(gfpath_err != 0) {
		kfree(new_obj);
		return gfpath_err;
	}

	struct loop_object *old_obj = 
		rhashtable_lookup_get_insert_fast(&loops_ht, &new_obj->linkage, loops_ht_params);
	
	if(IS_ERR(old_obj)) {
		kfree(new_obj);
		pr_err("%s: rhashtable_lookup_get_insert_fast(...) failed, error=%ld\n", 
			module_name(THIS_MODULE), PTR_ERR(old_obj));
		return -EFAULT;
	} else if(old_obj != NULL) {
		kfree(new_obj);
		return -EEXIST;
	}

	return 0;
}

static unsigned long get_block_magic(const struct block_device* b) {
	// https://www.linuxquestions.org/questions/showthread.php?p=6490737#post6490737
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)
	struct super_block *sb = (struct super_block*) b->bd_holder;
#else
	struct super_block *sb = b->bd_super;
#endif

	return sb->s_magic;
}

static int try_to_insert_block_device(const struct block_device* bdev) {
	unsigned long magic = get_block_magic(bdev);
	if(!bdsnap_has_supported_fs(magic)) {
		return -EBADTYPE;
	}

	struct blkdev_object *new_obj = kzalloc(sizeof(struct blkdev_object), GFP_KERNEL);
	if(new_obj == NULL) {
		print_kmalloc_failed();
		return -ENOMEM;
	}

	new_obj->magic = magic;
	new_obj->key = bdev->bd_dev;

	struct blkdev_object *old_obj = 
		rhashtable_lookup_get_insert_fast(&blkdevs_ht, &new_obj->linkage, blkdevs_ht_params);
		
	if(IS_ERR(old_obj)) {
		kfree(new_obj);
		pr_err("%s: rhashtable_lookup_get_insert_fast(...) failed, error=%ld\n", 
			module_name(THIS_MODULE), PTR_ERR(old_obj));
		return -EFAULT;
	} else if(old_obj != NULL) {
		kfree(new_obj);
		return -EEXIST;
	}

	return 0;
}

int register_device(const char* path) {
	struct inode *ino;
	int err = get_inode_from_path(path, &ino);
	if(err) {
		return err;
	}
	
	if(S_ISBLK(ino->i_mode)) {
		struct block_device *bdev = I_BDEV(ino);
		if(MAJOR(ino->i_rdev) == LOOP_MAJOR) {
			char* loop_backing_path = get_loop_device_backing_file(bdev);
			int err = try_to_insert_loop_device(loop_backing_path);
			kfree(loop_backing_path);
			return err;
		} else {
			return try_to_insert_block_device(bdev);
		}
	} else if(S_ISREG(ino->i_mode)) {
		return try_to_insert_loop_device(path);
	}

	return -EINVAL;
}

static int try_to_remove_loop_device(const char* path) {
	char *full_path = (char*) kzalloc(sizeof(char) * PATH_MAX, GFP_KERNEL);
	if(full_path == NULL) {
		print_kmalloc_failed();
		return -ENOMEM;
	}

	rcu_read_lock();

	struct loop_object *cur_obj = 
		rhashtable_lookup_fast(&loops_ht, full_path, loops_ht_params);

	kfree(full_path);

	if (
		cur_obj != NULL && 
		rhashtable_remove_fast(&loops_ht, &cur_obj->linkage, loops_ht_params) == 0
	) {
		kfree_rcu(cur_obj, rcu);
	}

	rcu_read_unlock();

	return cur_obj != NULL;
}

static int try_to_remove_block_device(const struct block_device* bdev) {
	rcu_read_lock();

	struct blkdev_object *cur_obj = 
		rhashtable_lookup_fast(&blkdevs_ht, &bdev->bd_dev, blkdevs_ht_params);

	if (
		cur_obj != NULL && 
		rhashtable_remove_fast(&blkdevs_ht, &cur_obj->linkage, blkdevs_ht_params) == 0
	) {
		kfree_rcu(cur_obj, rcu);
	}

	rcu_read_unlock();

	return cur_obj != NULL;
}

int unregister_device(const char* path) {
	struct inode *ino;
	int err = get_inode_from_path(path, &ino);
	if(err) {
		return err;
	}
	
	if(S_ISBLK(ino->i_mode)) {
		struct block_device *bdev = I_BDEV(ino);
		if(MAJOR(ino->i_rdev) == LOOP_MAJOR) {
			char* loop_backing_path = get_loop_device_backing_file(bdev);
			int err = try_to_remove_loop_device(loop_backing_path);
			kfree(loop_backing_path);
			return err;
		} else {
			return try_to_remove_block_device(bdev);
		}
	} else if(S_ISREG(ino->i_mode)) {
		return try_to_remove_loop_device(path);
	}

	return -EINVAL;
}

bool setup_devices(void) {
	if(rhashtable_init(&blkdevs_ht, &blkdevs_ht_params) != 0) {
		return false;
	}

	if(rhashtable_init(&loops_ht, &loops_ht_params) != 0) {
		rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
		return false;
	}

	return true;
}

void destroy_devices(void) {
	rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
	rhashtable_free_and_destroy(&loops_ht, loops_ht_free_fn, NULL);
}
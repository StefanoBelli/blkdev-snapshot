#include <linux/rhashtable.h>
#include <linux/namei.h>
#include <linux/major.h>

#include <devices.h>
#include <pr-err-failure.h>
#include <get-loop-backing-file.h>

/**
 * 
 * commmon across both "pure" block device and loop device
 *
 */

// "data" should not be visible at the time of init
static void __init_object_data(struct object_data* data, const char* wqfmt, const void *wqarg) {
	spin_lock_init(&data->lock);

	data->e.n_currently_mounted = 0;
	data->e.cached_blocks = NULL;
	data->e.d_snapdir = NULL;

	data->wq = alloc_ordered_workqueue(wqfmt, WQ_FREEZABLE, wqarg);
}

static void init_object_data_blkdev(struct object_data* data, dev_t devt) {
	__init_object_data(data, "bdsnap-b%d", (void*) (intptr_t) devt);
}

static void init_object_data_loop(struct object_data* data, const char* lof) {
	__init_object_data(data, "bdsnap-l%s", lof);
}

static void __cleanup_object_data(struct object_data* data, bool locking) {
	if(locking) {
		spin_lock(&data->lock);
	}

	flush_workqueue(data->wq);
	destroy_workqueue(data->wq);

	//epoch_destroy_cached_blocks_lru(data->e);
	//epoch_destroy_d_snapdir_dentry(data->e);
	
	if(locking) {
		spin_unlock(&data->lock);
	}
}

static void cleanup_object_data(struct object_data* data) {
	__cleanup_object_data(data, true);
}

static void cleanup_object_data_nolocking(struct object_data* data) {
	__cleanup_object_data(data, false);
}

/**
 *
 * "pure" block devices rhashtable
 *
 * as a reference: https://lwn.net/Articles/751374/
 *
 */

struct blkdev_object {
	struct rhash_head linkage;
	struct rcu_head rcu;

	dev_t key;
	struct object_data value;
};

static const struct rhashtable_params blkdevs_ht_params = {
	.key_len = sizeof(dev_t),
	.key_offset = offsetof(struct blkdev_object, key),
	.head_offset = offsetof(struct blkdev_object, linkage)
};

static struct rhashtable blkdevs_ht;

static void blkdevs_ht_free_fn(void* ptr, void* arg) {
	struct blkdev_object *bdptr = (struct blkdev_object*) ptr;
	cleanup_object_data(&bdptr->value);
	kfree_rcu(bdptr, rcu);
}

/**
 *
 * loop devices rhashtable
 *
 */

struct loop_object {
	struct rhash_head linkage;
	struct rcu_head rcu;

	char key[PATH_MAX + 1];
	struct object_data value;
};

static const struct rhashtable_params loops_ht_params = {
	.key_len = sizeof(char) * PATH_MAX,
	.key_offset = offsetof(struct loop_object, key),
	.head_offset = offsetof(struct loop_object, linkage)
};

static struct rhashtable loops_ht;

static void loops_ht_free_fn(void* ptr, void* arg) {
	struct loop_object *loptr = (struct loop_object*) ptr;
	cleanup_object_data(&loptr->value);
	kfree_rcu(loptr, rcu);
}

/**
 *
 * common utils
 *
 */

static int get_full_path(const char* path, char* out_full_path, size_t len) {
	struct path _path;
	int err = kern_path(path, LOOKUP_FOLLOW, &_path);
	if (err) {
		return err;
	}

	void* _ptr_in_buf = d_path(&_path, out_full_path, len);
	if(IS_ERR(_ptr_in_buf)) {
		pr_err_failure_with_code("d_path", PTR_ERR(_ptr_in_buf));
		path_put(&_path);
		return PTR_ERR(_ptr_in_buf);
	}

	path_put(&_path);

	return 0;
}

// IMPORTANT, REFCOUNTING:
// the output inode (out_inode) is grabbed
// if retval != 0, inode is not grabbed
static int get_inode_from_cstr_path(const char *path, struct inode **out_inode) {
	struct path _path;
	int err = kern_path(path, LOOKUP_FOLLOW, &_path);
	if (err) {
		return err;
	}

	*out_inode = igrab(_path.dentry->d_inode);

	path_put(&_path);

	return 0;
}

static int get_loop_device_backing_file(dev_t bddevt, char* out_lofname) {

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0)
	struct file* f_bd = bdev_file_open_by_dev(bddevt, BLK_OPEN_READ, NULL, NULL);
	if(IS_ERR(f_bd)) {
		pr_err_failure_with_code("bdev_file_open_by_dev", PTR_ERR(f_bd));
		return PTR_ERR(f_bd);
	}

	struct block_device *bd = file_bdev(f_bd);
#elif KERNEL_VERSION(6,6,23) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,9,0)
	struct bdev_handle *h_bd = bdev_open_by_dev(bddevt, BLK_OPEN_READ, NULL, NULL);
	if(IS_ERR(h_bd)) {
		pr_err_failure_with_code("bdev_open_by_dev", PTR_ERR(h_bd));
		return PTR_ERR(h_bd);
	}

	struct block_device *bd = h_bd->bdev;
#elif KERNEL_VERSION(6,5,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,6,23)
	struct block_device *bd = blkdev_get_by_dev(bddevt, BLK_OPEN_READ, NULL, NULL);
	if(IS_ERR(bd)) {
		pr_err_failure_with_code("blkdev_get_by_dev", PTR_ERR(bd));
		return PTR_ERR(bd);
	}
#elif KERNEL_VERSION(2,6,38) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0)
	struct block_device *bd = blkdev_get_by_dev(bddevt, FMODE_READ, NULL);
	if(IS_ERR(bd)) {
		pr_err_failure_with_code("blkdev_get_by_dev", PTR_ERR(bd));
		return PTR_ERR(bd);
	}
#else
	return -ENOSYS;
#endif

	const char* bkfile = get_loop_backing_file(bd);
	memcpy(out_lofname, bkfile, __MY_LO_NAME_SIZE);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0)
	bdev_fput(f_bd);
#elif KERNEL_VERSION(6,6,23) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,9,0)
	bdev_release(h_bd);
#elif KERNEL_VERSION(6,5,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,6,23)
	blkdev_put(bd, NULL);
#else
	blkdev_put(bd, FMODE_READ);
#endif

	return 0;
}

/**
 *
 * common code between insertion and removal
 * this reduces useless code duplication and
 * improves maintainability
 *
 */

static int __do_device_reging_operation(
		const char* path, 
		int (*op_on_loopdev)(const char*), 
		int (*op_on_blkdev)(dev_t)) {

	struct inode *ino;
	int err = get_inode_from_cstr_path(path, &ino);

	if(err != 0) {
		return err;
	}

	if(ino == NULL) {
		return -ENFILE;
	}

	if(S_ISBLK(ino->i_mode)) {
		if(MAJOR(ino->i_rdev) == LOOP_MAJOR) {
			char loop_backing_path[__MY_LO_NAME_SIZE + 1];
			loop_backing_path[__MY_LO_NAME_SIZE] = 0;

			err = get_loop_device_backing_file(ino->i_rdev, loop_backing_path);
			if(err == 0) {
				err = op_on_loopdev(loop_backing_path);
			}
		} else {
			err = op_on_blkdev(ino->i_rdev);
		}
	} else if(S_ISREG(ino->i_mode)) {
		err = op_on_loopdev(path);
	} else {
		err = -EINVAL;
	}

	iput(ino);
	return err;
}

/**
 *
 * insertion of devices
 *
 * context of freeing in case of rhashtable error is way different here than
 * in "try_to_remove_*" or "rhashtable_free_and_destroy": if error occoured
 * data is not visible to other threads - no need for RCU or anything, just
 * free data
 *
 */

static int try_to_insert_loop_device(const char* path) {
	struct loop_object *new_obj = kzalloc(sizeof(struct loop_object), GFP_KERNEL);
	if(new_obj == NULL) {
		pr_err_failure("kzalloc");
		return -ENOMEM;
	}

	int gfpath_err = get_full_path(path, new_obj->key, PATH_MAX);
	if(gfpath_err != 0) {
		kfree(new_obj);
		return gfpath_err;
	}

	init_object_data_loop(&new_obj->value, new_obj->key);

	struct loop_object *old_obj = 
		rhashtable_lookup_get_insert_fast(&loops_ht, &new_obj->linkage, loops_ht_params);
	
	if(IS_ERR(old_obj)) {
		pr_err_failure_with_code("rhashtable_lookup_get_insert_fast", PTR_ERR(old_obj));
		cleanup_object_data_nolocking(&new_obj->value);
		kfree(new_obj);
		return -EFAULT;
	} else if(old_obj != NULL) {
		cleanup_object_data_nolocking(&new_obj->value);
		kfree(new_obj);
		return -EEXIST;
	}

	return 0;
}

static int try_to_insert_block_device(dev_t bddevt) {
	struct blkdev_object *new_obj = kzalloc(sizeof(struct blkdev_object), GFP_KERNEL);
	if(new_obj == NULL) {
		pr_err_failure("kzalloc");
		return -ENOMEM;
	}

	new_obj->key = bddevt;

	init_object_data_blkdev(&new_obj->value, bddevt);

	struct blkdev_object *old_obj = 
		rhashtable_lookup_get_insert_fast(&blkdevs_ht, &new_obj->linkage, blkdevs_ht_params);
		
	if(IS_ERR(old_obj)) {
		pr_err_failure_with_code("rhashtable_lookup_get_insert_fast", PTR_ERR(old_obj));
		cleanup_object_data_nolocking(&new_obj->value);
		kfree(new_obj);
		return -EFAULT;
	} else if(old_obj != NULL) {
		cleanup_object_data_nolocking(&new_obj->value);
		kfree(new_obj);
		return -EEXIST;
	}

	return 0;
}

int register_device(const char* path) {
	return __do_device_reging_operation(
			path, 
			try_to_insert_loop_device, 
			try_to_insert_block_device);
}

/**
 *
 * removal of devices
 *
 */

static int try_to_remove_loop_device(const char* path) {
	char *full_path = (char*) kzalloc(sizeof(char) * (PATH_MAX + 1), GFP_KERNEL);
	if(full_path == NULL) {
		pr_err_failure("kzalloc");
		return -ENOMEM;
	}

	int gfpath_err = get_full_path(path, full_path, PATH_MAX);
	if(gfpath_err != 0) {
		kfree(full_path);
		return gfpath_err;
	}

	rcu_read_lock();

	struct loop_object *cur_obj = 
		rhashtable_lookup_fast(&loops_ht, full_path, loops_ht_params);

	kfree(full_path);

	if(cur_obj == NULL) {
		rcu_read_unlock();
		return -ENOKEY;
	}

	if (rhashtable_remove_fast(&loops_ht, &cur_obj->linkage, loops_ht_params) == 0) {
		loops_ht_free_fn((void*)cur_obj, NULL);
		rcu_read_unlock();
	} else {
		rcu_read_unlock();
		return -ENOKEY;
	}

	return 0;
}

static int try_to_remove_block_device(dev_t bddevt) {
	rcu_read_lock();

	struct blkdev_object *cur_obj = 
		rhashtable_lookup_fast(&blkdevs_ht, &bddevt, blkdevs_ht_params);
		
	if(cur_obj == NULL) {
		rcu_read_unlock();
		return -ENOKEY;
	}

	if (rhashtable_remove_fast(&blkdevs_ht, &cur_obj->linkage, blkdevs_ht_params) == 0) {
		blkdevs_ht_free_fn((void*)cur_obj, NULL);
		rcu_read_unlock();
	} else {
		rcu_read_unlock();
		return -ENOKEY;
	}

	return 0;
}

int unregister_device(const char* path) {
	return __do_device_reging_operation(
			path,
			try_to_remove_loop_device,
			try_to_remove_block_device);
}

/**
 * 
 * device lookup, needs RCU protection
 *
 */

struct object_data *get_device_data(const struct mountinfo *minfo) {
	if(minfo->type == MOUNTINFO_DEVICE_TYPE_BLOCK) {
		return rhashtable_lookup_fast(&blkdevs_ht, &minfo->device.bdevt, blkdevs_ht_params);
	}

	return rhashtable_lookup_fast(&loops_ht, minfo->device.lo_fname, loops_ht_params);
}


/**
 * 
 * setup, only called from module init fn
 *
 */

int setup_devices(void) {
	if(rhashtable_init(&blkdevs_ht, &blkdevs_ht_params) != 0) {
		return -EINVAL;
	}

	if(rhashtable_init(&loops_ht, &loops_ht_params) != 0) {
		rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
		return -EINVAL;
	}

	return 0;
}

void destroy_devices(void) {
	rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
	rhashtable_free_and_destroy(&loops_ht, loops_ht_free_fn, NULL);
}

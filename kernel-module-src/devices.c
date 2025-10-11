#include <linux/rhashtable.h>
#include <linux/namei.h>

#include <lru-ng.h>
#include <devices.h>
#include <pr-err-failure.h>
#include <get-loop-backing-file.h>

/**
 * 
 * commmon across both "pure" block device and loop device
 *
 */

// "data" should not be visible at the time of init
static void __init_object_data(
		struct object_data* data, 
		const char* original_dev_name, 
		const char* wqfmt, 
		const void *wqarg) {

	spin_lock_init(&data->general_lock);
	spin_lock_init(&data->wq_destroy_lock);
	spin_lock_init(&data->cleanup_epoch_lock);

	data->e = NULL;

	strscpy(data->original_dev_name, original_dev_name, PATH_MAX);

	data->wq = alloc_ordered_workqueue(wqfmt, WQ_FREEZABLE, wqarg);
	data->wq_is_destroyed = false;
}

static void init_object_data_blkdev(
		struct object_data* data, 
		dev_t devt, 
		const char* original_dev_name) {

	__init_object_data(
			data, original_dev_name, 
			"bdsnap-b%d", (void*) (intptr_t) devt);
}

static void init_object_data_loop(
		struct object_data* data, 
		const char* lof, 
		const char* original_dev_name) {

	__init_object_data(
			data, original_dev_name, 
			"bdsnap-l%s", lof);
}

//this is called only once:
// - when module is unloaded, so rhashtable_free_and_destroy triggers the "freefn"
// - when user deactivates snapshot for a device
// - when we're unable to "insert device" and we need to cleanup (no locking as
//	 in this case, object_data is not visible in any way to other thrs)
//
// defer work: lie about wq destruction and epoch cleaning - it will be done later on
//an umount event won't be able to queue_work for epoch cleanup
//data->e will eventually be set to NULL and we got the general_lock
//on the contrary, if the umount event count gets the general_lock prior
//we just wait for all the wqs to finish and nothing more is done 
//since it already queued the last work that will cleanup the epoch 
//and they set data->e to NULL (see workfn)

struct waddw_args  {
	struct workqueue_struct *device_wq;
	struct epoch *last_epoch;
};

#define SET_WADDW_ARGS(_name, _wq, _epoch) \
	(_name).device_wq = (_wq); \
	(_name).last_epoch = (_epoch)

#define DEFINE_WADDW_ARGS(_name, _wq, _epoch) \
	struct waddw_args _name = { \
		.device_wq = (_wq), \
		.last_epoch = (_epoch) \
	}

static void __do_waddw(const struct waddw_args *wargs) {
	flush_workqueue(wargs->device_wq);
	destroy_workqueue(wargs->device_wq);

	if(wargs->last_epoch != NULL) {
		if(wargs->last_epoch->path_snapdir != NULL) {
			path_put(wargs->last_epoch->path_snapdir);
		}

		if(wargs->last_epoch->cached_blocks != NULL) {
			lru_ng_cleanup_and_destroy(wargs->last_epoch->cached_blocks);
		}

		kfree(wargs->last_epoch);
	}
}

struct waddw_work {
	struct waddw_args waddw_args;
	struct work_struct work;
};

static void wait_and_destroy_device_workqueue(struct work_struct *work) {
	struct waddw_work *args = container_of(work, struct waddw_work, work);
	__do_waddw(&args->waddw_args);
}

struct waddw_worklist_node {
	struct list_head node;
	struct waddw_work *wargs;
};

static LIST_HEAD(waddw_worklist);
static DEFINE_SPINLOCK(waddw_worklist_glock);

//atomic context allowed
static void cleanup_object_data(struct object_data* data) {
	unsigned long cpu_flags_0;
	spin_lock_irqsave(&data->general_lock, cpu_flags_0);

	unsigned long cpu_flags_1;
	spin_lock_irqsave(&data->wq_destroy_lock, cpu_flags_1);

	data->wq_is_destroyed = true;

	spin_unlock_irqrestore(&data->wq_destroy_lock, cpu_flags_1);

	struct epoch *saved_last_epoch = data->e;
	if(data->e != NULL) {
		data->e = NULL;
	}

	spin_unlock_irqrestore(&data->general_lock, cpu_flags_0);

	struct waddw_worklist_node *wlistnode = kmalloc(sizeof(struct waddw_worklist_node), GFP_ATOMIC);
	if(wlistnode == NULL) {
		return;
	}

	INIT_LIST_HEAD(&wlistnode->node);
	wlistnode->wargs = kmalloc(sizeof(struct waddw_work), GFP_ATOMIC);

	if(wlistnode->wargs == NULL) {
		return;
	}

	SET_WADDW_ARGS(wlistnode->wargs->waddw_args, data->wq, saved_last_epoch);
	INIT_WORK(&wlistnode->wargs->work, wait_and_destroy_device_workqueue);
	schedule_work(&wlistnode->wargs->work);

	spin_lock_irqsave(&waddw_worklist_glock, cpu_flags_0);

	list_add_tail(&wlistnode->node, &waddw_worklist);

	struct waddw_worklist_node *cur;
	struct waddw_worklist_node *tmp;

	list_for_each_entry_safe(cur, tmp, &waddw_worklist, node) {
		if(!work_busy(&cur->wargs->work)) {
			list_del(&cur->node);
			kfree(cur->wargs);
			kfree(cur);
		}
	}

	spin_unlock_irqrestore(&waddw_worklist_glock, cpu_flags_0);
}

//in process context only
static void cleanup_object_data_notvisible(struct object_data* data) {
	DEFINE_WADDW_ARGS(args, data->wq, data->e);
	__do_waddw(&args);
	data->wq_is_destroyed = true;
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
	.head_offset = offsetof(struct blkdev_object, linkage),
	.automatic_shrinking = true
};

static struct rhashtable blkdevs_ht;

static void blkdevs_ht_free_fn(void* ptr, void* __always_unused arg) {
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

	char *key;
	char __key_buf[PATH_MAX];
	struct object_data value;
};

static u32 loop_object_key_hashfn(const void *data, u32 __always_unused len, u32 seed) {
	return jhash(data, strlen((const char*) data), seed);
}

static u32 loop_object_obj_hashfn(const void *data, u32 __always_unused len, u32 seed) {
	const struct loop_object *lo = (const struct loop_object*) data;
	u32 hash = jhash(lo->key, strlen(lo->key), seed);

	return hash;
}

static int loop_object_obj_cmpfn(struct rhashtable_compare_arg *arg, const void *obj) {
	const char *new_key = (const char*) arg->key;
	int res = strcmp(((const struct loop_object*) obj)->key, new_key);

	return res;
}

static const struct rhashtable_params loops_ht_params = {
	.key_offset = offsetof(struct loop_object, key),
	.head_offset = offsetof(struct loop_object, linkage),
	.hashfn = loop_object_key_hashfn,
	.obj_hashfn = loop_object_obj_hashfn,
	.obj_cmpfn = loop_object_obj_cmpfn,
	.automatic_shrinking = true
};

static struct rhashtable loops_ht;

static void loops_ht_free_fn(void* ptr, void* __always_unused arg) {
	struct loop_object *loptr = (struct loop_object*) ptr;
	cleanup_object_data(&loptr->value);
	kfree_rcu(loptr, rcu);
}

/**
 *
 * common utils
 *
 */

static char* get_full_path(const char* path, char* out_full_path, size_t len) {
	struct path _path;
	int err = kern_path(path, LOOKUP_FOLLOW, &_path);
	if (err) {
		return ERR_PTR(err);
	}

	char* _ptr_in_buf = d_path(&_path, out_full_path, len);
	if(IS_ERR(_ptr_in_buf)) {
		pr_err_failure_with_code("d_path", PTR_ERR(_ptr_in_buf));
		path_put(&_path);
		return _ptr_in_buf;
	}

	path_put(&_path);

	return _ptr_in_buf;
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
	strscpy(out_lofname, bkfile, __MY_LO_NAME_SIZE);

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

static bool allow_reging_operation = false;
static DECLARE_RWSEM(allow_reging_operation_sem);

static int __do_device_reging_operation(
		const char* path, 
		int (*op_on_loopdev)(const char*, const char*), 
		int (*op_on_blkdev)(dev_t, const char*)) {

	down_read(&allow_reging_operation_sem);
	if(!allow_reging_operation) {
		up_read(&allow_reging_operation_sem);
		return -EBUSY;
	}

	struct inode *ino;
	int err = get_inode_from_cstr_path(path, &ino);

	if(err != 0) {
		up_read(&allow_reging_operation_sem);
		return err;
	}

	if(ino == NULL) {
		up_read(&allow_reging_operation_sem);
		return -ENFILE;
	}

	if(S_ISBLK(ino->i_mode)) {
		if(MAJOR(ino->i_rdev) == LOOP_MAJOR) {
			char loop_backing_path[__MY_LO_NAME_SIZE];

			err = get_loop_device_backing_file(ino->i_rdev, loop_backing_path);
			if(err == 0) {
				err = op_on_loopdev(loop_backing_path, path);
			}
		} else {
			err = op_on_blkdev(ino->i_rdev, path);
		}
	} else if(S_ISREG(ino->i_mode)) {
		err = op_on_loopdev(path, path);
	} else {
		err = -EINVAL;
	}

	iput(ino);
	up_read(&allow_reging_operation_sem);
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

static int try_to_insert_loop_device(const char* path, const char* original_dev_name) {
	struct loop_object *new_obj = kzalloc(sizeof(struct loop_object), GFP_KERNEL);
	if(new_obj == NULL) {
		pr_err_failure("kzalloc");
		return -ENOMEM;
	}

	new_obj->key = get_full_path(path, new_obj->__key_buf, PATH_MAX);
	if(IS_ERR(new_obj->key)) {
		kfree(new_obj);
		return PTR_ERR(new_obj->key);
	}

	init_object_data_loop(&new_obj->value, new_obj->key, original_dev_name);


	struct loop_object *old_obj = 
		rhashtable_lookup_get_insert_key(&loops_ht, new_obj->key, &new_obj->linkage, loops_ht_params);
	
	if(IS_ERR(old_obj)) {
		pr_err_failure_with_code("rhashtable_lookup_get_insert_key", PTR_ERR(old_obj));
		cleanup_object_data_notvisible(&new_obj->value);
		kfree(new_obj);
		return -EFAULT;
	} else if(old_obj != NULL) {
		cleanup_object_data_notvisible(&new_obj->value);
		kfree(new_obj);
		return -EEXIST;
	}

	return 0;
}

static int try_to_insert_block_device(dev_t bddevt, const char* original_dev_name) {
	struct blkdev_object *new_obj = kzalloc(sizeof(struct blkdev_object), GFP_KERNEL);
	if(new_obj == NULL) {
		pr_err_failure("kzalloc");
		return -ENOMEM;
	}

	new_obj->key = bddevt;

	init_object_data_blkdev(&new_obj->value, bddevt, original_dev_name);

	struct blkdev_object *old_obj = 
		rhashtable_lookup_get_insert_fast(&blkdevs_ht, &new_obj->linkage, blkdevs_ht_params);
		
	if(IS_ERR(old_obj)) {
		pr_err_failure_with_code("rhashtable_lookup_get_insert_key", PTR_ERR(old_obj));
		cleanup_object_data_notvisible(&new_obj->value);
		kfree(new_obj);
		return -EFAULT;
	} else if(old_obj != NULL) {
		cleanup_object_data_notvisible(&new_obj->value);
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

static int try_to_remove_loop_device(
		const char* path, const char* __always_unused arg) {

	//PATH_MAX is too big for the stack
	char *__full_path_buf = (char*) kmalloc(sizeof(char) * PATH_MAX, GFP_KERNEL);
	if(__full_path_buf == NULL) {
		pr_err_failure("kmalloc");
		return -ENOMEM;
	}

	char* full_path = get_full_path(path, __full_path_buf, PATH_MAX);
	if(IS_ERR(full_path)) {
		kfree(__full_path_buf);
		return PTR_ERR(full_path);
	}

	rcu_read_lock();

	struct loop_object *cur_obj = 
		rhashtable_lookup(&loops_ht, full_path, loops_ht_params);

	kfree(__full_path_buf);

	if(cur_obj == NULL) {
		rcu_read_unlock();
		return -ENOKEY;
	}

	if (rhashtable_remove_fast(&loops_ht, &cur_obj->linkage, loops_ht_params) == 0) {
		rcu_read_unlock();
		loops_ht_free_fn((void*)cur_obj, NULL);
	} else {
		rcu_read_unlock();
		return -ENOKEY;
	}

	return 0;
}

static int try_to_remove_block_device(
		dev_t bddevt, const char* __always_unused arg) {

	rcu_read_lock();

	struct blkdev_object *cur_obj = 
		rhashtable_lookup(&blkdevs_ht, &bddevt, blkdevs_ht_params);
		
	if(cur_obj == NULL) {
		rcu_read_unlock();
		return -ENOKEY;
	}

	if (rhashtable_remove_fast(&blkdevs_ht, &cur_obj->linkage, blkdevs_ht_params) == 0) {
		rcu_read_unlock();
		blkdevs_ht_free_fn((void*)cur_obj, NULL);
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

static struct object_data *__do_get_device_data_always(const void* key, bool is_block) {
	if(is_block) {
		struct blkdev_object *bo = 
			rhashtable_lookup(&blkdevs_ht, key, blkdevs_ht_params);

		if(bo == NULL) {
			return NULL;
		}

		return &bo->value;
	}

	struct loop_object *lo = 
		rhashtable_lookup(&loops_ht, key, loops_ht_params);

	if(lo == NULL) {
		return NULL;
	}

	return &lo->value;
}

struct object_data *get_device_data_always(const struct mountinfo *minfo) {
	if(minfo->type == MOUNTINFO_DEVICE_TYPE_BLOCK) {
		return __do_get_device_data_always(&minfo->device.bdevt, true);
	}

	return __do_get_device_data_always(minfo->device.lo_fname, false);
}


/**
 * 
 * setup, only called from module init fn
 *
 */

int setup_devices(void) {
	down_write(&allow_reging_operation_sem);

	if(rhashtable_init(&blkdevs_ht, &blkdevs_ht_params) != 0) {
		up_write(&allow_reging_operation_sem);
		return -EINVAL;
	}

	if(rhashtable_init(&loops_ht, &loops_ht_params) != 0) {
		rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
		up_write(&allow_reging_operation_sem);
		return -EINVAL;
	}

	allow_reging_operation = true;
	up_write(&allow_reging_operation_sem);

	return 0;
}

void destroy_devices(void) {
	down_write(&allow_reging_operation_sem);
	allow_reging_operation = false;

	rhashtable_free_and_destroy(&blkdevs_ht, blkdevs_ht_free_fn, NULL);
	rhashtable_free_and_destroy(&loops_ht, loops_ht_free_fn, NULL);

	// must do blocking flush_work on cleanup_object_data's generated work
	// no other way. Otherwise module code execution attempt will result 
	// in kernel oops (accompanied with a smiling page fault)
	
	//unsigned long cpu_flags;
	//spin_lock_irqsave(&waddw_worklist_glock, cpu_flags);

	struct waddw_worklist_node *cur;
	struct waddw_worklist_node *tmp;

	list_for_each_entry_safe(cur, tmp, &waddw_worklist, node) {
		flush_work(&cur->wargs->work);

		list_del(&cur->node);
		kfree(cur->wargs);
		kfree(cur);
	}

	//spin_unlock_irqrestore(&waddw_worklist_glock, cpu_flags);

	up_write(&allow_reging_operation_sem);
}

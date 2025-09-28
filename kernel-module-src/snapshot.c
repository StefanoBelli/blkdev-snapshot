#include <bdsnap/bdsnap.h>

#include <devices.h>

static void ensure_d_snapdir(struct dentry **d_snapdir) {

}

struct make_snapshot_work {
	sector_t block_nr;
	unsigned blocksize;
	char* block;
	struct dentry *d_snapdir;
	struct list_lru *cached_blocks;
	char original_dev_name[PATH_MAX];
	char first_mount_date[MNT_FMT_DATE_LEN + 1];
	struct work_struct work;
};

static void make_snapshot(struct work_struct *work) {
	struct make_snapshot_work *msw_args =
		container_of(work, struct make_snapshot_work, work);

	if(msw_args->cached_blocks == NULL) {
		msw_args->cached_blocks = kmalloc(sizeof(struct list_lru), GFP_KERNEL);
		if(msw_args->cached_blocks == NULL) {
			goto __make_snapshot_finish0;
		}

		if(list_lru_init(msw_args->cached_blocks) != 0) {
			kfree(msw_args->cached_blocks);
			goto __make_snapshot_finish0;
		}
	}

__make_snapshot_finish0:
	kfree(msw_args->block);
	kfree(msw_args);
}

static bool queue_snapshot_work(
		struct object_data *obj, const char* blk, 
		sector_t blknr, unsigned blksize) {

	struct make_snapshot_work *msw = 
		 kmalloc(sizeof(struct make_snapshot_work), GFP_ATOMIC);
	if(msw == NULL) {
		return false;
	}

	msw->block = kmalloc(sizeof(char) * blksize, GFP_ATOMIC);
	if(msw->block == NULL) {
		kfree(msw);
		return false;
	}

	INIT_WORK(&msw->work, make_snapshot);
	msw->block_nr = blknr;
	msw->blocksize = blksize;
	msw->d_snapdir = obj->e.d_snapdir;
	msw->cached_blocks = obj->e.cached_blocks;
	memcpy(msw->first_mount_date, obj->e.first_mount_date, MNT_FMT_DATE_LEN + 1);
	strscpy(msw->original_dev_name, obj->original_dev_name, PATH_MAX);
	memcpy(msw->block, blk, sizeof(char) * blksize);

	bool retval;
	if(!(retval = queue_work(obj->wq, &msw->work))) {
		kfree(msw->block);
		kfree(msw);
	}

	return retval;
}

/**
 *
 * exported fns, the ones which the FS-specific part implementor should use
 *
 */

void* bdsnap_search_device(
		const struct block_device* bdev, 
		unsigned long *saved_cpu_flags) {

	struct mountinfo minfo;
	from_block_device_to_mountinfo(&minfo, bdev);

	struct object_data *data = get_device_data_always(&minfo);
	if(data == NULL) {
		return NULL;
	}

	spin_lock_irqsave(&data->cleanup_epoch_lock, *saved_cpu_flags);

	// umount with MNT_DETACH may lose data
	if(unlikely(data->e.n_currently_mounted == 0)) {
		spin_unlock_irqrestore(&data->cleanup_epoch_lock, *saved_cpu_flags);
		return NULL;
	}

	if(data->wq_is_destroyed || spin_is_locked(&data->wq_destroy_lock)) {
		spin_unlock_irqrestore(&data->cleanup_epoch_lock, *saved_cpu_flags);
		return NULL;
	}

	return (void*) data;
}

EXPORT_SYMBOL_GPL(bdsnap_search_device);

bool bdsnap_make_snapshot(
		void* handle, const char* block, 
		sector_t blocknr, unsigned blocksize, 
		unsigned long cpu_flags) {

	struct object_data *data = (struct object_data*) handle;
	bool ret = false;

	if(data != NULL && !data->wq_is_destroyed && !spin_is_locked(&data->wq_destroy_lock)) {
		spin_lock(&data->wq_destroy_lock);
		if(!data->wq_is_destroyed) {
			ret = queue_snapshot_work(data, block, blocknr, blocksize);
		}
		spin_unlock(&data->wq_destroy_lock);
	}

	spin_unlock_irqrestore(&data->cleanup_epoch_lock, cpu_flags);
	return ret;
}

EXPORT_SYMBOL_GPL(bdsnap_make_snapshot);

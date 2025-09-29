#include <linux/namei.h>
#include <linux/dcache.h>

#include <bdsnap/bdsnap.h>

#include <devices.h>

/**
 * 
 * mkdir
 *
 */

static inline struct dentry* __do_vfs_mkdir(struct mnt_idmap *idmap, struct inode *i_par, struct dentry *d_new) {
	inode_lock(i_par);
	struct dentry *d = vfs_mkdir(idmap, i_par, d_new, 0600);
	inode_unlock(i_par);
	return d;
}

static inline struct dentry* mkdir_via_dent_by_dent(struct dentry *d_new, struct dentry *d_parent, struct mnt_idmap *idmap) {
	struct inode *ino = d_inode(d_parent);
	return __do_vfs_mkdir(idmap, ino, d_new);
}

static inline struct dentry* mkdir_via_name_by_dent(const char* dir_name, struct dentry *d_parent, struct mnt_idmap *idmap) {
	struct dentry *d_new = d_alloc_name(d_parent, dir_name);
	struct dentry *rv = mkdir_via_dent_by_dent(d_new, d_parent, idmap);
	
	if(IS_ERR(rv) || rv != d_new) {
		d_invalidate(d_new);
	}
	
	return rv;
}

/**
 *
 * mkdir snapshot dir
 *
 */

static bool init_path_snapdir(struct path **path_snapdir, const char *snap_subdir_name) {
	struct path root_path;
	if(kern_path("/", 0, &root_path) != 0) {
		return false;
	}

	if(mnt_want_write(root_path.mnt) != 0) {
		path_put(&root_path);
		return false;
	}

	struct mnt_idmap *idmap = mnt_idmap(root_path.mnt);

	struct dentry *d_snap = mkdir_via_name_by_dent("snapshot", root_path.dentry, idmap);
	dget(d_snap);

	struct dentry *d_sub = mkdir_via_name_by_dent(snap_subdir_name, d_snap, idmap);

	mnt_drop_write(root_path.mnt);
	dput(d_snap);

	*path_snapdir = kmalloc(sizeof(struct path), GFP_KERNEL);
	if(*path_snapdir == NULL) {
		path_put(&root_path);
		return false;
	}

	(*path_snapdir)->dentry = d_sub;
	(*path_snapdir)->mnt = root_path.mnt;

	path_get(*path_snapdir);
	path_put(&root_path);

	return true;
}

static bool ensure_path_snapdir_ok(struct path **path_snapdir, const char* subdirname) {
	if(likely(*path_snapdir != NULL)) {
		struct dentry *dent = (*path_snapdir)->dentry;
		if(likely(d_is_positive(dent) && d_is_dir(dent))) {
			return true;
		}

		struct dentry *dent_par = dget_parent(dent);
		if(likely(d_is_positive(dent_par) && d_is_dir(dent_par))) {
			struct mnt_idmap *idmap = mnt_idmap((*path_snapdir)->mnt);
			if(mnt_want_write((*path_snapdir)->mnt) == 0) {
				(*path_snapdir)->dentry = mkdir_via_dent_by_dent(dent, dent_par, idmap);
				mnt_drop_write((*path_snapdir)->mnt);
				return !IS_ERR((*path_snapdir)->dentry);
			}
		}

		path_put(*path_snapdir);
		*path_snapdir = NULL;

		return ensure_path_snapdir_ok(path_snapdir, subdirname);
	} else {
		return init_path_snapdir(path_snapdir, subdirname);
	}
}

/**
 *
 * initiating lru
 *
 */

static inline bool ensure_cached_blocks_lru_ok(struct list_lru **lru) {
	if(unlikely(*lru == NULL)) {
		*lru = kmalloc(sizeof(struct list_lru), GFP_KERNEL);
		if(unlikely(*lru == NULL)) {
			return false;
		}

		if(unlikely(list_lru_init(*lru) != 0)) {
			kfree(*lru);
			return false;
		}
	}

	return true;
}


struct make_snapshot_work {
	sector_t block_nr;
	unsigned blocksize;
	char* block;
	struct path *path_snapdir;
	struct list_lru *cached_blocks;
	char original_dev_name[PATH_MAX];
	char first_mount_date[MNT_FMT_DATE_LEN + 1];
	struct work_struct work;
};

static void make_snapshot(struct work_struct *work) {
	struct make_snapshot_work *msw_args =
		container_of(work, struct make_snapshot_work, work);

	if(!ensure_cached_blocks_lru_ok(&msw_args->cached_blocks)) {
		goto __make_snapshot_finish0;
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
	msw->path_snapdir = obj->e.path_snapdir;
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

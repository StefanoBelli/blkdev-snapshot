#include <linux/version.h>
#include <linux/namei.h>

#include <bdsnap/bdsnap.h>

#include <devices.h>
#include <lru.h>
#include <pr-err-failure.h>

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

static inline void path_snapdir_get(struct path **path_snapdir, struct dentry *dent, struct vfsmount *mnt) {
	//this will be handed over to next work
	//until a deactivation or epoch change
	(*path_snapdir)->dentry = dent;
	(*path_snapdir)->mnt = mnt;
	path_get(*path_snapdir);
}

static struct dentry* mkdir_may_exist(const char* relname, struct dentry* dentry, struct vfsmount *mnt) {
	struct path out_path;

	if(vfs_path_lookup(dentry, mnt, relname, 0, &out_path) == 0) {
		dget(out_path.dentry);
		path_put(&out_path);
		return out_path.dentry;
	}

	if(unlikely(mnt_want_write(mnt) != 0)) {
		pr_err_failure("mnt_want_write");
		return NULL;
	}

	struct dentry *d_new = mkdir_via_name_by_dent(relname, dentry, mnt_idmap(mnt));

	mnt_drop_write(mnt);

	if(IS_ERR(d_new)) {
		pr_err_failure_with_code("vfs_mkdir", PTR_ERR(d_new));
		return NULL;
	}

	return d_new;
}

static bool init_path_snapdir(struct path **path_snapdir, const char *snap_subdir_name) {
	bool rv = false;
	struct path root_path;

	int kern_path_err = kern_path("/", 0, &root_path);

	if(unlikely(kern_path_err != 0)) {
		pr_err_failure_with_code("kern_path", kern_path_err);
		return false;
	}

	struct dentry *d_snap = mkdir_may_exist("snapshot", root_path.dentry, root_path.mnt);

	if(d_snap == NULL) {
		pr_err_failure("mkdir_may_exist");
		goto __init_path_snapdir_finish0;
	}

	struct dentry *d_sub = mkdir_may_exist(snap_subdir_name, d_snap, root_path.mnt);

	dput(d_snap);

	if(d_sub == NULL) {
		pr_err_failure("mkdir_may_exist");
		goto __init_path_snapdir_finish0;
	}

	*path_snapdir = kmalloc(sizeof(struct path), GFP_KERNEL);
	if(unlikely(*path_snapdir == NULL)) {
		pr_err_failure("kmalloc");
		goto __init_path_snapdir_finish1;
	}

	rv = true;
	path_snapdir_get(path_snapdir, d_sub, root_path.mnt);

__init_path_snapdir_finish1:
	dput(d_sub);
__init_path_snapdir_finish0:
	path_put(&root_path);
	return rv;
}

static bool ensure_path_snapdir_ok(struct path **path_snapdir, const char* devname, const char* mountdate) {
	if(likely(*path_snapdir != NULL)) {
		struct dentry *dent = (*path_snapdir)->dentry;
		if(likely(d_is_positive(dent) && d_is_dir(dent))) {
			return true;
		}

		pr_warn("%s: existing snapblock directory is broken (devname=%s, mountdate=%s)\n",
				module_name(THIS_MODULE), devname, mountdate);

		path_put(*path_snapdir);
		kfree(*path_snapdir);
		*path_snapdir = NULL;

		return ensure_path_snapdir_ok(path_snapdir, devname, mountdate);
	} else {
		size_t subdirname_len_wnul = strlen(devname) + MNT_FMT_DATE_LEN + 1;
		char *subdirname = kmalloc(subdirname_len_wnul, GFP_KERNEL);
		snprintf(subdirname, subdirname_len_wnul, "%s%s", devname, mountdate);
		bool res = init_path_snapdir(path_snapdir, subdirname);
		kfree(subdirname);
		return res;
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

/**
 *
 * snapblock file mgmt
 *
 */

#define SNAPBLOCK_MAGIC 0x5ade5aad5abe5aef

enum snapblock_payload_type : u64 {
	SNAPBLOCK_PAYLOAD_TYPE_RAW,
};

//trying to have a 64-bit word memalign
struct snapblock_file_hdr {
	u64 magic;
	u64 blknr;
	u64 payldsiz;
	enum snapblock_payload_type payld_type;
	u64 payld_off;
} __packed;

#define DECLARE_SNAPBLOCK_FILE_HDR( \
		_mand_hdr_name, \
		__block_num, \
		__payload_size) \
		\
	struct snapblock_file_hdr _mand_hdr_name; \
	(_mand_hdr_name).magic = SNAPBLOCK_MAGIC; \
	(_mand_hdr_name).blknr = (__block_num); \
	(_mand_hdr_name).payldsiz = (__payload_size); \
	(_mand_hdr_name).payld_type = SNAPBLOCK_PAYLOAD_TYPE_RAW; \
	(_mand_hdr_name).payld_off = sizeof(struct snapblock_file_hdr)

static int read_snapblock_mandatory_header(struct file *filp, 
		struct snapblock_file_hdr *out_hdr) {

	size_t nrbytes = sizeof(struct snapblock_file_hdr);

	loff_t pos;
	size_t read_bytes = kernel_read(filp, (char*) out_hdr, nrbytes, &pos);

	if(read_bytes != nrbytes) {
		return -ENODATA;
	}

	if(out_hdr->magic != SNAPBLOCK_MAGIC) {
		return -EINVAL;
	}

	return 0;
}

struct snapblock_file_write_args {
	const struct snapblock_file_hdr* mandatory_hdr;
	const void* extended_hdr;
	size_t extended_hdr_size;
	const void* payload;
	size_t payload_size;
};

#define DECLARE_SNAPBLOCK_FILE_WRITE_ARGS( \
		_args_name, \
		__mand_hdr, \
		__payload, \
		__payload_size) \
		\
	struct snapblock_file_write_args _args_name; \
	(_args_name).mandatory_hdr = (__mand_hdr); \
	(_args_name).extended_hdr = NULL; \
	(_args_name).extended_hdr_size = 0; \
	(_args_name).payload = ((const void*)(__payload)); \
	(_args_name).payload_size = (__payload_size)

static int create_snapblock_file(u64 blknr, 
		struct file **out_filp, const struct path *path_snapdir) {

	char *namebuf = kmalloc(PATH_MAX, GFP_KERNEL);
	if(namebuf == NULL) {
		return -ENOMEM;
	}

	snprintf(namebuf, PATH_MAX, "snapblock-%lld", blknr);

	struct dentry *d_new = d_alloc_name(path_snapdir->dentry, namebuf);
	if(d_new == NULL) {
		kfree(namebuf);
		return -ENODATA;
	}

	kfree(namebuf);

	struct mnt_idmap *par_idmap = mnt_idmap(path_snapdir->mnt);
	struct inode *par_ino = d_inode(path_snapdir->dentry);

	int err = vfs_create(par_idmap, par_ino, d_new, FMODE_READ, true);
	if(err != 0) {
		return -ECHILD;
	}

	struct path path_new_snapblock = {
		.dentry = d_new,
		.mnt = path_snapdir->mnt
	};

	*out_filp = dentry_open(&path_new_snapblock, O_RDONLY, current->cred);
	if(IS_ERR(*out_filp)) {
		return -EACCES;
	}

	return 0;
}

static int write_snapblock(const struct path *path_snapdir,
		const struct snapblock_file_write_args *wargs) {

	struct file *filp;
	int rv = 0;
	u64 blknr = wargs->mandatory_hdr->blknr;

	rv = create_snapblock_file(blknr, &filp, path_snapdir);

	if(rv != 0) {
		return rv;
	}

	loff_t off;
	size_t wrote;

	wrote = kernel_write(
			filp, wargs->mandatory_hdr, sizeof(struct snapblock_file_hdr), &off);

	if(wrote != sizeof(struct snapblock_file_hdr)) {
		rv = -EINVAL;
		goto __write_snapblock_finish0;
	}

	if(wargs->extended_hdr != NULL && wargs->extended_hdr_size > 0) {
		wrote = kernel_write(
				filp, wargs->extended_hdr, wargs->extended_hdr_size, &off);

		if(wrote != wargs->extended_hdr_size) {
			rv = -EINVAL;
			goto __write_snapblock_finish0;
		}
	}

	wrote = kernel_write(
			filp,wargs->payload, wargs->payload_size, &off);

	if(wrote != wargs->payload_size) {
		rv = -EINVAL;
	}

__write_snapblock_finish0:
	fput(filp);
	return rv;
}

/**
 *
 * directory lookup
 *
 */

struct lookup_dir_args {
	sector_t blknr;
	bool found;
	struct path *path_snapdir;
	struct dir_context ctx;
};

static bool lookup_dir_iter_cb(
		struct dir_context *ctx, const char* item, 
		int __always_unused namelen, loff_t __always_unused offset, 
		u64 __always_unused ino, unsigned int d_type) {

	struct lookup_dir_args *lkargs = 
		container_of(ctx, struct lookup_dir_args, ctx);

	if(d_type != DT_REG) {
		return true;
	}

	struct dentry *dent = lkargs->path_snapdir->dentry;
	struct vfsmount *vfsm = lkargs->path_snapdir->mnt;

	struct path path_snapblock;
	int err = vfs_path_lookup(dent, vfsm, item, 0, &path_snapblock);
	if(err != 0) {
		return true;
	}

	struct file *fsnapblock = dentry_open(&path_snapblock, O_RDONLY, current->cred);

	bool res = true;

	struct snapblock_file_hdr hdr;
	if(read_snapblock_mandatory_header(fsnapblock, &hdr) == 0) {
		if(hdr.magic == lkargs->blknr) {
			lkargs->found = true;
			res = false;
		}
	}

	fput(fsnapblock);
	path_put(&path_snapblock);

	return res;
}

static int lookup_dir(struct path *path_snapdir, sector_t blknr, bool *out_found) {
	*out_found = false;

	struct file *fdir = dentry_open(path_snapdir, O_RDONLY, current->cred);
	if(IS_ERR(fdir)) {
		return PTR_ERR(fdir);
	}

	struct lookup_dir_args lkargs = {
		.blknr = blknr,
		.found = false,
		.ctx = {
			.actor = lookup_dir_iter_cb,
			.pos = 0
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0)
			,.count = 0
#endif
		},
		.path_snapdir = path_snapdir
	};

	int err = iterate_dir(fdir, &lkargs.ctx);
	if(err != 0) {
		goto __lookup_dir_finish0;
	}

	*out_found = lkargs.found;

__lookup_dir_finish0:
	fput(fdir);
	return err;
}

/**
 *
 * snapshot deferred work
 *
 */

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

	if(!ensure_cached_blocks_lru_ok(
				&msw_args->cached_blocks)) {
		goto __make_snapshot_finish0;
	}

	if(lookup_lru(msw_args->cached_blocks, msw_args->block_nr)) {
		goto __make_snapshot_finish0;
	}

	if(!ensure_path_snapdir_ok(
				&msw_args->path_snapdir, 
				msw_args->original_dev_name, 
				msw_args->first_mount_date)) {
		goto __make_snapshot_finish0;
	}

	bool dir_found;
	if(lookup_dir(msw_args->path_snapdir,msw_args->block_nr, &dir_found) != 0) {
		goto __make_snapshot_finish0;
	}

	if(!dir_found) {
		DECLARE_SNAPBLOCK_FILE_HDR(file_hdr, msw_args->block_nr, msw_args->blocksize);
		DECLARE_SNAPBLOCK_FILE_WRITE_ARGS(wargs, &file_hdr, msw_args->block, msw_args->blocksize);
		if(write_snapblock(msw_args->path_snapdir, &wargs) != 0) {
			goto __make_snapshot_finish0;
		}
	}

	add_lru(msw_args->cached_blocks, msw_args->block_nr);
__make_snapshot_finish0:
	kfree(msw_args->block);
	kfree(msw_args);
}

/**
 *
 * deferred work caller
 *
 */

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

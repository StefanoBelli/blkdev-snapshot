#include <linux/version.h>
#include <linux/namei.h>

#if KERNEL_VERSION(5,12,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
#include <linux/mount.h>
#endif

#include <bdsnap/bdsnap.h>

#include <devices.h>
#include <pr-err-failure.h>

/**
 *
 * dentry cache alloc
 *
 */

// base (aka parent) inode needs to be locked!
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0)

#	define new_dentry(_name, _base, _mnt) \
		(lookup_one(mnt_idmap(_mnt), &QSTR(_name), _base))

#elif KERNEL_VERSION(3,15,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)

#	define new_dentry(_name, _base, ...) \
		(lookup_one_len(_name, _base, strlen(_name)))

#else

#	error we cant help (non-exported lookup_one_len), maybe upgrade?

#endif

/**
 * 
 * mkdir core
 *
 */

// allocates an entirely-new dentry to attach to the parent one
// since we allocated it, if vfs_mkdir complains then we invalidate it
// no need to dget on the newly-allocated dentry
// requires mnt_want_write
static inline struct dentry* mkdir_via_name_by_dent(const char* dir_name, struct dentry *d_parent, struct vfsmount *mnt) {
	struct inode *ino = d_inode(d_parent);

	inode_lock(ino);

	struct dentry *d_new = new_dentry(dir_name, d_parent, mnt);
	if(IS_ERR(d_new)) {
		pr_err_failure_with_code("new_dentry", PTR_ERR(d_new));
		inode_unlock(ino);
		return NULL;
	}

	umode_t dirmode = 0700;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,15,0)
	d_new = vfs_mkdir(mnt_idmap(mnt), ino, d_new, dirmode);
#elif KERNEL_VERSION(6,3,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,15,0)
	int err = vfs_mkdir(mnt_idmap(mnt), ino, d_new, dirmode);
#elif KERNEL_VERSION(5,12,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
	int err = vfs_mkdir(mnt_user_ns(mnt), ino, d_new, dirmode);
#elif KERNEL_VERSION(3,15,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	int err = vfs_mkdir(ino, d_new, dirmode);
#else
#	error too old of a system... (vfs_mkdir is non-exported)
#endif

	inode_unlock(ino);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,15,0)
	if(IS_ERR(d_new)) {
		pr_err_failure_with_code("vfs_mkdir", PTR_ERR(d_new));
	}
#else
	if(err != 0) {
		pr_err_failure_with_code("vfs_mkdir", err);
	}
#endif

	return d_new;
}

/**
 *
 * mkdir snapshot dir
 *
 */

// this will be handed over to next work
// until a deactivation or epoch change	
static inline void path_snapdir_get(struct path **path_snapdir, struct dentry *dent, struct vfsmount *mnt) {
	(*path_snapdir)->dentry = dent;
	(*path_snapdir)->mnt = mnt;
	path_get(*path_snapdir);
}

// this tries to obtain a dentry if fs object exists,
// otherwise tries to mkdir
static struct dentry* mkdir_may_exist(const char* relname, struct dentry* dentry, struct vfsmount *mnt) {
	struct path existing_path;
	if(vfs_path_lookup(dentry, mnt, relname, 0, &existing_path) == 0) {
		dget(existing_path.dentry);
		path_put(&existing_path);

		if(!d_is_dir(existing_path.dentry)) {
			pr_err("%s: **PAY ATTENTION HERE**\n"
					"found existing object, \"%s\", "
					"expecting it to be a directory, but it is not.\n"
					"This is a human-made mistake.\n"
					"Manual intervention is required:\n"
					"issuing \"rm %s\" (from cwd of containing dir)\n"
					"should be enough to allow auto fixing\n",
					module_name(THIS_MODULE), relname, relname);
			return NULL;
		}

		return existing_path.dentry;
	}

	if(unlikely(mnt_want_write(mnt) != 0)) {
		pr_err_failure("mnt_want_write");
		return NULL;
	}

	struct dentry *d_new = mkdir_via_name_by_dent(relname, dentry, mnt);

	mnt_drop_write(mnt);

	if(IS_ERR(d_new)) {
		pr_err_failure("mkdir_via_name_by_dent");
		return NULL;
	}

	return d_new;
}

// this traverses "/", "snapshot/", "<orig_dev_name>-<timestamp>/"
// allocates a struct path* when everything is done (via kmalloc),
// sets its fields and then does a path_get on it. This instance will be
// handed over to the next work and so on.
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

// this is called to ensure a "healthy" struct path
// if everything looks good then checks are fast and lightweight
// recursion depth is *VERY* limited: just one call
static bool ensure_path_snapdir_ok(struct path **path_snapdir, const char* devname, const char* mountdate) {
	if(likely(*path_snapdir != NULL)) {
		struct dentry *dent = (*path_snapdir)->dentry;
		if(likely(
					d_really_is_positive(dent) && 
					d_inode(dent)->i_nlink > 0 &&
					d_is_dir(dent))) {

			return true;
		}

		pr_warn("%s: existing snapblock directory is broken "
				"(devname=%s, mountdate=%s), trying automatic fixing...\n",
				module_name(THIS_MODULE), devname, mountdate);

		path_put(*path_snapdir);
		kfree(*path_snapdir);
		*path_snapdir = NULL;

		return ensure_path_snapdir_ok(path_snapdir, devname, mountdate);
	} else {
		size_t subdirname_len_wnul = strlen(devname) + MNT_FMT_DATE_LEN + 1;

		char *subdirname = kmalloc(subdirname_len_wnul, GFP_KERNEL);
		if(subdirname == NULL) {
			pr_err_failure("kmalloc");
			return false;
		}

		snprintf(subdirname, subdirname_len_wnul, "%s%s", kbasename(devname), mountdate);

		// here, if res is false:
		//	* no kmalloc of path_snapdir went through
		//	* no path_get of path_snapdir
		//	* path_snapdir ptr is still NULL
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

static inline bool ensure_cached_blocks_lru_ok(struct lru_ng **lru) {
	if(unlikely(*lru == NULL)) {
		return (*lru = lru_ng_alloc_and_init()) != NULL;
	}

	return true;
}

/**
 *
 * snapblock file mgmt
 *
 */

#define SNAPBLOCK_MAGIC 0x5ade5aad5abe5aef

// in conjunction with extended header this allows
// to implement schemes like encrypt-than-mac, AEAD, 
// simple integrity checksums or whatever you want
enum snapblock_payload_type : u64 {
	SNAPBLOCK_PAYLOAD_TYPE_RAW,
};

// this is the mandatory header, self-explainatory
//trying to have a 64-bit word memalign
struct snapblock_file_hdr {
	u64 magic;
	u64 blknr;
	u64 payldsiz;
	enum snapblock_payload_type payld_type;
	u64 payld_off;
} __packed;

#define DEFINE_SNAPBLOCK_FILE_HDR( \
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

static inline bool read_snapblock_mandatory_header(
		struct file *filp, 
		struct snapblock_file_hdr *out_hdr,
		loff_t start_hdr_off) {

	size_t nrbytes = sizeof(struct snapblock_file_hdr);

	loff_t pos = start_hdr_off;
	size_t read_bytes = kernel_read(filp, (char*) out_hdr, nrbytes, &pos);

	if(read_bytes != nrbytes) {
		return false;
	}

	if(out_hdr->magic != SNAPBLOCK_MAGIC) {
		return false;
	}

	return true;
}

static bool create_snapblocks_file(struct file **out_filp, const struct path *path_snapdir) {
	struct inode *par_ino = d_inode(path_snapdir->dentry);

	inode_lock(par_ino);

	struct dentry *d_new = new_dentry("snapblocks", path_snapdir->dentry, path_snapdir->mnt);

	if(IS_ERR(d_new)) {
		pr_err_failure_with_code("new_dentry", PTR_ERR(d_new));
		inode_unlock(par_ino);
		return false;
	}

	umode_t mode = 0600;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
	int err = vfs_create(mnt_idmap(path_snapdir->mnt), par_ino, d_new, mode, true);
#elif KERNEL_VERSION(5,12,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
	int err = vfs_create(mnt_user_ns(path_snapdir->mnt), par_ino, d_new, mode, true);
#elif KERNEL-VERSION(3,15,0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	int err = vfs_create(par_ino, d_new, mode, true);
#else
#	error missing vfs_create (non-exported symbol)
#endif

	inode_unlock(par_ino);

	if(err != 0) {
		pr_err_failure_with_code("vfs_create", err);
		dput(d_new);
		return false;
	}

	//no need to path_get or anything here
	struct path path_new_snapblock = {
		.dentry = d_new,
		.mnt = path_snapdir->mnt
	};

	*out_filp = dentry_open(&path_new_snapblock, O_RDWR | O_APPEND, current->cred);

	dput(d_new);

	if(IS_ERR(*out_filp)) {
		pr_err_failure_with_code("dentry_open", PTR_ERR(*out_filp));
		return false;
	}

	return true;
}

// these are just the arguments passed to the write routine
// for snapblocks, not the file content itself
struct write_snapblock_args {
	const struct snapblock_file_hdr* mandatory_hdr;
	const void* extended_hdr;
	size_t extended_hdr_size;
	const void* payload;
	size_t payload_size;
};

#define DEFINE_WRITE_SNAPBLOCK_ARGS( \
		_args_name, \
		__mand_hdr, \
		__payload, \
		__payload_size) \
		\
	struct write_snapblock_args _args_name; \
	(_args_name).mandatory_hdr = (__mand_hdr); \
	(_args_name).extended_hdr = NULL; \
	(_args_name).extended_hdr_size = 0; \
	(_args_name).payload = ((const void*)(__payload)); \
	(_args_name).payload_size = (__payload_size)

static bool write_snapblock(struct file *filp, const struct write_snapblock_args *wargs) {
	size_t wrote;
	bool rv = false;

	size_t mandatory_hdr_size = sizeof(struct snapblock_file_hdr);

	wrote = kernel_write(
			filp, wargs->mandatory_hdr, mandatory_hdr_size, NULL);
	if(wrote != mandatory_hdr_size) {
		pr_err_failure_with_code("kernel_write", wrote);
		goto __write_snapblock_finish0;
	}

	if(wargs->extended_hdr != NULL && wargs->extended_hdr_size > 0) {

		wrote = kernel_write(
				filp, wargs->extended_hdr, wargs->extended_hdr_size, NULL);
		if(wrote != wargs->extended_hdr_size) {
			pr_err_failure_with_code("kernel_write", wrote);
			goto __write_snapblock_finish0;
		}
	}

	wrote = kernel_write(
			filp, wargs->payload, wargs->payload_size, NULL);
	if(wrote != wargs->payload_size) {
		pr_err_failure_with_code("kernel_write", wrote);
		goto __write_snapblock_finish0;
	}

	rv = true;

__write_snapblock_finish0:
	return rv;
}

static bool file_lookup(u64 blknr, struct file *filp) {
	struct snapblock_file_hdr blk_header;
	loff_t foff = 0;

	while(read_snapblock_mandatory_header(filp, &blk_header, foff)) {
		if(blk_header.blknr == blknr) {
			return true;
		}

		foff += blk_header.payld_off + blk_header.payldsiz;
	}

	return false;
}

/**
 *
 * ensure snapblocks file ok
 *
 */

static bool ensure_snapblocks_file_ok(const struct path *path_snapdir, struct file **out_filp) {
	struct path path_snapblocks;

	if(vfs_path_lookup(path_snapdir->dentry, path_snapdir->mnt, "snapblocks", 0, &path_snapblocks) == 0) {
		if(!d_is_file(path_snapblocks.dentry)) {
			pr_err("%s: **PAY ATTENTION HERE**\n"
					"found existing object, \"snapblocks\", "
					"expecting it to be a regular file, but it is not.\n"
					"This is a human-made mistake.\n"
					"Manual intervention is required:\n"
					"issuing \"rm snapblocks [...whatever rmflags needed here...]\" "
					"(from cwd of containing dir)\n"
					"should be enough to allow auto fixing\n",
					module_name(THIS_MODULE));
			return false;
		}

		*out_filp = dentry_open(&path_snapblocks, O_RDWR | O_APPEND, current->cred);
		path_put(&path_snapblocks);

		return true;
	}

	return create_snapblocks_file(out_filp, path_snapdir);
}
/**
 *
 * snapshot deferred work
 *
 */

struct make_snapshot_work {
	sector_t block_nr;
	u64 blocksize;
	char* block;
	struct path **path_snapdir;
	struct lru_ng **cached_blocks;
	char original_dev_name[PATH_MAX];
	char first_mount_date[MNT_FMT_DATE_LEN + 1];
	struct work_struct work;
};

static void make_snapshot(struct work_struct *work) {
	struct make_snapshot_work *msw_args =
		container_of(work, struct make_snapshot_work, work);

	if(!ensure_cached_blocks_lru_ok(
				msw_args->cached_blocks)) {
		goto __make_snapshot_finish0;
	}

	if(lru_ng_lookup(
				*msw_args->cached_blocks, 
				msw_args->block_nr)) {
		goto __make_snapshot_finish0;
	}

	if(!ensure_path_snapdir_ok(
				msw_args->path_snapdir, 
				msw_args->original_dev_name, 
				msw_args->first_mount_date)) {
		goto __make_snapshot_finish0;
	}

	struct file *snapblocks_filp;
	if(!ensure_snapblocks_file_ok(
				*msw_args->path_snapdir,
				&snapblocks_filp)) {
		goto __make_snapshot_finish0;
	}

	if(file_lookup(
				msw_args->block_nr, 
				snapblocks_filp))  {
		goto __make_snapshot_finish2;
	}

	DEFINE_SNAPBLOCK_FILE_HDR(file_hdr, 
			msw_args->block_nr, 
			msw_args->blocksize);

	DEFINE_WRITE_SNAPBLOCK_ARGS(wargs, 
			&file_hdr, 
			msw_args->block, 
			msw_args->blocksize);

	if(write_snapblock(
				snapblocks_filp, 
				&wargs) != 0) {
		goto __make_snapshot_finish1;
	}

__make_snapshot_finish2:
	lru_ng_add(*msw_args->cached_blocks, msw_args->block_nr);
__make_snapshot_finish1:
	fput(snapblocks_filp);
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

	if(obj->e == NULL) {
		//should never happen, but who knows...
		return false;
	}

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
	msw->path_snapdir = &obj->e->path_snapdir;
	msw->cached_blocks = &obj->e->cached_blocks;
	memcpy(msw->first_mount_date, obj->e->first_mount_date, MNT_FMT_DATE_LEN + 1);
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

bool bdsnap_test_device(
		const struct block_device* bdev) {

	struct mountinfo minfo;
	from_block_device_to_mountinfo(&minfo, bdev);

	rcu_read_lock();

	struct object_data *data = get_device_data_always(&minfo);
	if(data == NULL) {
		rcu_read_unlock();
		return false;
	}

	bool validity = false;

	unsigned long flags;
	spin_lock_irqsave(&data->cleanup_epoch_lock, flags);
	
	validity = 
		data->e != NULL && 
		data->e->n_currently_mounted > 0 && 
		!data->wq_is_destroyed && 
		!spin_is_locked(&data->wq_destroy_lock);

	rcu_read_unlock();

	spin_unlock_irqrestore(&data->cleanup_epoch_lock, flags);

	return validity;
}

EXPORT_SYMBOL_GPL(bdsnap_test_device);

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

	bool not_valid =
		data->e == NULL ||
		data->e->n_currently_mounted == 0 ||
		data->wq_is_destroyed ||
		spin_is_locked(&data->wq_destroy_lock);

	if(unlikely(not_valid)) {
		spin_unlock_irqrestore(&data->cleanup_epoch_lock, *saved_cpu_flags);
		return NULL;
	}

	return (void*) data;
}

EXPORT_SYMBOL_GPL(bdsnap_search_device);

bool bdsnap_make_snapshot(
		void* handle, const char* block, 
		sector_t blocknr, u64 blocksize, 
		unsigned long cpu_flags) {

	struct object_data *data = (struct object_data*) handle;
	bool ret = false;

	bool valid = 
		data != NULL && 
		!data->wq_is_destroyed && 
		!spin_is_locked(&data->wq_destroy_lock);

	if(likely(valid)) {
		unsigned long flags;
		spin_lock_irqsave(&data->wq_destroy_lock, flags);
		if(!data->wq_is_destroyed) {
			ret = queue_snapshot_work(data, block, blocknr, blocksize);
		}
		spin_unlock_irqrestore(&data->wq_destroy_lock, flags);
		spin_unlock_irqrestore(&data->cleanup_epoch_lock, cpu_flags);
	}

	return ret;
}

EXPORT_SYMBOL_GPL(bdsnap_make_snapshot);

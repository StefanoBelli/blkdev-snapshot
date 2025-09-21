#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#include <supportfs.h>

struct __supported_fs {
	struct bdsnap_supported_fs fs;
	struct list_head node;
};

static LIST_HEAD(supported_fss);
static DEFINE_MUTEX(g_lock);
static int allows_registering = 1;

static inline struct __supported_fs *fs_is_already_supported(unsigned long srch_magic) {
	struct __supported_fs *pos;
	list_for_each_entry(pos, &supported_fss, node) {
		if(pos->fs.magic == srch_magic) {
			return pos;
		}
	}

	return NULL;
}

int bdsnap_register_supported_fs(const struct bdsnap_supported_fs* fs) {
	if(mutex_lock_interruptible(&g_lock) != 0) {
		return BDSNAP_ERR_REG_MTXLCKEINTR;
	}

	if (!allows_registering) {
		mutex_unlock(&g_lock);
		return BDSNAP_ERR_REG_CANNOTREG;
	}

	if(fs_is_already_supported(fs->magic) != NULL) {
		mutex_unlock(&g_lock);
		return BDSNAP_ERR_REG_ALREADYSUPP;
	}

	struct __supported_fs *newly_supported_fs = 
		(struct __supported_fs*) kmalloc(sizeof(struct __supported_fs), GFP_KERNEL);
	
	if(newly_supported_fs == NULL) {
		mutex_unlock(&g_lock);
		return BDSNAP_ERR_REG_MEMEXHAUSTED;
	}

	memcpy(&newly_supported_fs->fs, fs, sizeof(struct bdsnap_supported_fs));

	if(newly_supported_fs->fs.init_support()) {
		kfree(newly_supported_fs);
		mutex_unlock(&g_lock);
		return BDSNAP_ERR_REG_INITFAIL;
	}

	if(newly_supported_fs->fs.owner != NULL && newly_supported_fs->fs.owner != THIS_MODULE) {
		if(!try_module_get(newly_supported_fs->fs.owner)) {
			newly_supported_fs->fs.finish_support();
			kfree(newly_supported_fs);
			mutex_unlock(&g_lock);
			return BDSNAP_ERR_REG_MODGET;
		}
	}

	list_add(&newly_supported_fs->node, &supported_fss);
	mutex_unlock(&g_lock);

	return BDSNAP_ERR_REG_OK;
}

EXPORT_SYMBOL_GPL(bdsnap_register_supported_fs);

int bdsnap_cleanup_supported_fs(void) {
	if(mutex_lock_interruptible(&g_lock) != 0) {
		return BDSNAP_ERR_CLEANUP_MTXLCKEINTR;
	}

	if(!allows_registering) {
		mutex_unlock(&g_lock);
		return BDSNAP_ERR_CLEANUP_ALREADYDONE;
	}
	
	allows_registering = 0;
	
	struct __supported_fs *pos;
	struct __supported_fs *tmp;

	list_for_each_entry_safe(pos, tmp, &supported_fss, node) {
		pos->fs.finish_support();
		if(pos->fs.owner != NULL && pos->fs.owner != THIS_MODULE) {
			module_put(pos->fs.owner);
		}
		list_del(&pos->node);
		kfree(pos);
	}

	return BDSNAP_ERR_CLEANUP_OK;
}

bool bdsnap_has_supported_fs(unsigned long magic) {
	if(mutex_lock_interruptible(&g_lock) != 0) {
		return false;
	}

	if(!allows_registering) {
		mutex_unlock(&g_lock);
		return false;
	}

	bool has_support = fs_is_already_supported(magic) != NULL;
	mutex_unlock(&g_lock);

	return has_support;
}

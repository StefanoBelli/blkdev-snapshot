#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cred.h>
#include <crypto/hash.h>
#include <activation.h>

static struct crypto_shash *sha256_shash = NULL;
static char *auth_passwd;

static int auth_check(const char* passwd);

/* activate/deactivate snapshot */

static int activate_snapshot(const char* dev_name, const char* passwd) {
    int auth_check_rv = auth_check(passwd);
    if(auth_check_rv != 0) {
        return auth_check_rv;
    }

    return 0;
}

static int deactivate_snapshot(const char* dev_name, const char* passwd) {
    int auth_check_rv = auth_check(passwd);
    if(auth_check_rv != 0) {
        return auth_check_rv;
    }


    return 0;
}

/* compute sha256 */

static int hash_sha256(const char* data, size_t datalen, char* output) {
    struct shash_desc *desc;

    size_t size = sizeof(struct shash_desc) + crypto_shash_descsize(sha256_shash);
    desc = kmalloc(size, GFP_KERNEL);
    if (desc == NULL) {
        return -ENOMEM;
    }
    
    desc->tfm = sha256_shash;

    return crypto_shash_digest(desc, data, datalen, output);
}
/* password cmp */

static int password_cmp(const char* passwd) {
    if(sha256_shash != NULL) {
        char hashed_passwd[32];
        int rv = hash_sha256(passwd, strlen(passwd), hashed_passwd);
        if(rv != 0) {
            pr_err("hash_sha256 has failed: %d\n", rv);
            return rv;
        }
        return memcmp(hashed_passwd, auth_passwd, 32);
    } else {
        return strcmp(passwd, auth_passwd);
    }
}

/* auth check */

static int auth_check(const char* passwd) {
    const struct cred* task_cred = current->cred;
    if(task_cred->euid.val != 0) {
        return -EPERM;
    }
    
    if(password_cmp(passwd) != 0) {
        return -EACCES;
    }

    return 0;
}


/* sysfs-related things below... */

static inline int parse_call_args(const char* data, size_t datalen, const char** lhs, const char** rhs) {
    if(data[datalen - 1] != 0) {
        return -EINVAL;
    }

    char* separator = strchr(data, '\r');
    if(separator == NULL) {
        return -EINVAL;
    }

    *separator = 0;

    *lhs = data;
    *rhs = separator + 1;

    return 0; 
}

typedef int(*wrapped_call_fnt)(const char*, const char*);

static int call_wrapper(const char* data, size_t datalen, wrapped_call_fnt callback) {
    const char *devn;
    const char *pwd;

    if(parse_call_args(data, datalen, &devn, &pwd) != 0) {
        return -EINVAL;
    }

    int retval = callback(devn, pwd);
    if(retval) {
        return retval;
    }

    return datalen;   
}

static ssize_t activate_snapshot_sysfs_store(
        __always_unused struct kobject*, 
        __always_unused struct kobj_attribute*, 
        const char* data, size_t datalen) {

    return call_wrapper(data, datalen, activate_snapshot);
}

static ssize_t deactivate_snapshot_sysfs_store(
        __always_unused struct kobject*, 
        __always_unused struct kobj_attribute*, 
        const char* data, size_t datalen) {

    return call_wrapper(data, datalen, deactivate_snapshot);
}

static const struct kobj_attribute activate_kobj_attribute = (struct kobj_attribute) {
    .show = NULL,
    .store = activate_snapshot_sysfs_store,
    .attr = (struct attribute) {
        .mode = S_IWUSR | S_IWGRP | S_IWOTH,
        .name = "activate_snapshot"
    }
};

static const struct kobj_attribute deactivate_kobj_attribute = (struct kobj_attribute) {
    .show = NULL,
    .store = deactivate_snapshot_sysfs_store,
    .attr = (struct attribute) {
        .mode = S_IWUSR | S_IWGRP | S_IWOTH,
        .name = "deactivate_snapshot"
    }
};

extern char* activation_ct_passwd;

int setup_activation_mechanism_via_sysfs(void) {
    struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

    if(sysfs_create_file(this_module_kobj, &activate_kobj_attribute.attr) != 0) {
        return -1;
    }

    if(sysfs_create_file(this_module_kobj, &deactivate_kobj_attribute.attr) != 0) {
        sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
        return -1;
    }
    
    sha256_shash = crypto_alloc_shash("sha256", 0, 0);
    if (!IS_ERR(sha256_shash)) {
        auth_passwd = kmalloc(32, GFP_KERNEL);
        if(auth_passwd == NULL) {
            crypto_free_shash(sha256_shash);
            return -ENOMEM;
        }

        if(hash_sha256(activation_ct_passwd, strlen(activation_ct_passwd), auth_passwd) != 0) {
            crypto_free_shash(sha256_shash);
            kfree(auth_passwd);
            return -1;
        }
    } else {
        pr_warn("unable to allocate sha256 shash: %ld\n", PTR_ERR(sha256_shash));
        auth_passwd = kmalloc(strlen(activation_ct_passwd), GFP_KERNEL);
        if(auth_passwd == NULL) {
            return -ENOMEM;
        }
        memcpy(auth_passwd, activation_ct_passwd, strlen(activation_ct_passwd));
    }

    memset(activation_ct_passwd, 0, strlen(activation_ct_passwd));

    return 0;
}

void destroy_activation_mechanism_via_sysfs(void) {
    kfree(auth_passwd);

    if (sha256_shash != NULL) {
        crypto_free_shash(sha256_shash); 
    }

    struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

    sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
    sysfs_remove_file(this_module_kobj, &deactivate_kobj_attribute.attr);
}
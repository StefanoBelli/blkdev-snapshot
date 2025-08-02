#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cred.h>
#include <activation.h>

static int activate_snapshot(const char* dev_name, const char* passwd) {
    const struct cred* task_cred = current->cred;
    if(task_cred->euid.val != 0) {
        return -EPERM;
    }

    return 0;
}

static int deactivate_snapshot(const char* dev_name, const char* passwd) {
    const struct cred* task_cred = current->cred;
    if(task_cred->euid.val != 0) {
        return -EPERM;
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

int setup_activation_mechanism_via_sysfs(void) {
    struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

    if(sysfs_create_file(this_module_kobj, &activate_kobj_attribute.attr) != 0) {
        return -1;
    }

    if(sysfs_create_file(this_module_kobj, &deactivate_kobj_attribute.attr) != 0) {
        sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
        return -1;
    }

    return 0;
}

void destroy_activation_mechanism_via_sysfs(void) {
    struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

    sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
    sysfs_remove_file(this_module_kobj, &deactivate_kobj_attribute.attr);
}
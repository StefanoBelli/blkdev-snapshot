#include <linux/cred.h>

#ifndef CONFIG_SYSFS
#include <linux/fs.h>
#endif

#include <passwd.h>
#include <devices.h>
#include <activation.h>
#include <pr-err-failure.h>


static int auth_check(const char* passwd);

/* activate/deactivate snapshot */

static int activate_snapshot(const char* dev_name, const char* passwd) {
	int auth_check_rv = auth_check(passwd);
	if(auth_check_rv != 0) {
		return auth_check_rv;
	}

	return register_device(dev_name);
}

static int deactivate_snapshot(const char* dev_name, const char* passwd) {
	int auth_check_rv = auth_check(passwd);
	if(auth_check_rv != 0) {
		return auth_check_rv;
	}

	return unregister_device(dev_name);
}

/* ---- */

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

static inline int parse_call_args(char* data, size_t datalen, const char** lhs, const char** rhs) {
	if(data[datalen - 1] != 0) {
		return -EINVAL;
	}

	char* separator = strchr(data, '\r');
	if(separator == NULL) {
		return -EINVAL;
	}

	*separator = 0;

	char* first_nowschr = strim(data);
	if(*first_nowschr == 0) {
		return -EINVAL;
	}

	*lhs = first_nowschr;
	*rhs = separator + 1;

	return 0; 
}

typedef int(*wrapped_call_fnt)(const char*, const char*);

static int call_wrapper(char* data, size_t datalen, wrapped_call_fnt callback) {
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

#ifdef CONFIG_SYSFS

static ssize_t __sysfs_call_wrapper(const char* data, size_t datalen, wrapped_call_fnt fn) {
	char* buf = kmalloc(sizeof(char) * datalen, GFP_KERNEL);
	if(buf == NULL) {
		pr_err_failure("kmalloc");
		return -ENOMEM;
	}

	memcpy(buf, data, sizeof(char) * datalen);

	ssize_t rv = call_wrapper(buf, datalen, fn);

	kfree(buf);

	return rv;
}

static ssize_t activate_snapshot_sysfs_store(
		__always_unused struct kobject*, 
		__always_unused struct kobj_attribute*, 
		const char* data, size_t datalen) {

	return __sysfs_call_wrapper(data, datalen, activate_snapshot);
}

static ssize_t deactivate_snapshot_sysfs_store(
		__always_unused struct kobject*, 
		__always_unused struct kobj_attribute*, 
		const char* data, size_t datalen) {

	return __sysfs_call_wrapper(data, datalen, deactivate_snapshot);
}

static const struct kobj_attribute activate_kobj_attribute = (struct kobj_attribute) {
	.store = activate_snapshot_sysfs_store,
	.attr = (struct attribute) {
		.mode = S_IWUSR | S_IWGRP | S_IWOTH,
		.name = "activate_snapshot",
	}
};

static const struct kobj_attribute deactivate_kobj_attribute = (struct kobj_attribute) {
	.store = deactivate_snapshot_sysfs_store,
	.attr = (struct attribute) {
		.mode = S_IWUSR | S_IWGRP | S_IWOTH,
		.name = "deactivate_snapshot"
	}
};

#else

#define ACTIVATION_CHRDEV_NAME "blkdev-snapshot-activation"
#define ACTIVATE_CHRDEV_IOCTL_CMD 0
#define DEACTIVATE_CHRDEV_IOCTL_CMD 1

struct activation_ioctl_args {
	const char* data;
	size_t datalen;
};

static long activation_chrdev_ioctl(struct file* f, unsigned int cmd, unsigned long arg) {
	if(cmd != ACTIVATE_CHRDEV_IOCTL_CMD && cmd != DEACTIVATE_CHRDEV_IOCTL_CMD) {
		return -EINVAL;
	}

	struct activation_ioctl_args * __user user_args = 
		(struct activation_ioctl_args* __user) arg;

	char *buf = kmalloc(sizeof(char) * user_args->datalen, GFP_KERNEL);
	if(buf == NULL) {
		pr_err_failure("kmalloc");
		return -ENOMEM;
	}

	if(copy_from_user(buf, user_args->data, user_args->datalen) != 0) {
		kfree(buf);
		return -EFAULT;
	}

	wrapped_call_fnt fun = 
		cmd == ACTIVATE_CHRDEV_IOCTL_CMD ? 
		activate_snapshot : 
		deactivate_snapshot;

	int rv = call_wrapper(buf, user_args->datalen, fun);

	kfree(buf);

	return rv;
}

static int activation_chrdev_maj;
static const struct file_operations activation_chrdev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = activation_chrdev_ioctl
};

extern unsigned int activation_dev_req_maj;

#endif

int setup_activation_mechanism(void) {
	int rv = setup_passwd();
	if(rv != 0) {
		return rv;
	}

#ifdef CONFIG_SYSFS
	struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

	if((rv = sysfs_create_file(this_module_kobj, &activate_kobj_attribute.attr)) != 0) {
		pr_err_failure_with_code("sysfs_create_file", rv);
		destroy_passwd();
		return rv;
	}

	if((rv = sysfs_create_file(this_module_kobj, &deactivate_kobj_attribute.attr)) != 0) {
		pr_err_failure_with_code("sysfs_create_file", rv);
		sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
		destroy_passwd();
		return rv;
	}
#else
	activation_chrdev_maj = register_chrdev(activation_dev_req_maj, 
			ACTIVATION_CHRDEV_NAME, &activation_chrdev_fops);

	if(activation_chrdev_maj < 0) {
		pr_err_failure_with_code("register_chrdev", activation_chrdev_maj);
		unregister_chrdev(activation_chrdev_maj, ACTIVATION_CHRDEV_NAME);
		destroy_passwd();
		return activation_chrdev_maj;
	} else if(activation_dev_req_maj == 0) {
		pr_info("%s: activation device for blkdev snapshot got major number: %d\n", 
				module_name(THIS_MODULE), activation_chrdev_maj);
	}
#endif

	return 0;
}

void destroy_activation_mechanism(void) {

#ifdef CONFIG_SYSFS
	struct kobject *this_module_kobj = &THIS_MODULE->mkobj.kobj;

	sysfs_remove_file(this_module_kobj, &activate_kobj_attribute.attr);
	sysfs_remove_file(this_module_kobj, &deactivate_kobj_attribute.attr);
#else
	unregister_chrdev(activation_chrdev_maj, ACTIVATION_CHRDEV_NAME);
#endif

	destroy_passwd();
}

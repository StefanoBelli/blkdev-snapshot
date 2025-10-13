#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

static void print_help(const char* prog, const char* msg) {
	if(msg != NULL) {
		fprintf(stderr, "args-error: %s\n", msg);
	}

	printf("usage: %s [-h] [-a or -d] [-c chrdev or -s] <-f device or file> <-p password>\n", prog);
	puts(" -a: activate snapshot service for device (not mandatory, default)");
	puts(" -d: deactivate snapshot service for device (not mandatory)");
	puts(" -c: use *that* character device as an interface to the snapshot kernel module (not mandatory)");
	puts(" -s: use sysfs as the interface to the snapshot kernel module (not mandatory, default)");
	puts(" -f: the block device or regular image file (mandatory)");
	puts(" -p: the password (mandatory)");
	puts(" -h: to print this help (not mandatory)");
}

static char* concat(const char* a, const char* b) {
	size_t s = strlen(a) + strlen(b) + 2;

	char* buf = (char*) malloc(sizeof(char) * s);
	if(buf == NULL) {
		return NULL;
	}

	memset(buf, 0, sizeof(char) * s);
	snprintf(buf, s, "%s\r%s", a, b);

	return buf;
}

static void __do_sysfs(const char* sysfs_path, const char* path, const char* passwd) {
	int fd = open(sysfs_path, O_WRONLY);
	if(fd < 0) {
		perror("open");
		return;
	}

	char* str = concat(path, passwd);

	int err = write(fd, str, strlen(str) + 1);
	if(err < 0) {
		perror("write");
	}

	close(fd);
	free(str);
}

#define __gcc_unused __attribute__((__unused__))

static void activate_sysfs(const char* path, const char* passwd, __gcc_unused const char* u0) {
	const char *sysfs_path = "/sys/module/blkdev_snapshot/activate_snapshot";
	__do_sysfs(sysfs_path, path, passwd);
}

static void deactivate_sysfs(const char* path, const char* passwd, __gcc_unused const char* u0) {
	const char *sysfs_path = "/sys/module/blkdev_snapshot/deactivate_snapshot";
	__do_sysfs(sysfs_path, path, passwd);
}

#undef __gcc_unused

#define ACTIVATE_CHRDEV_IOCTL_CMD 0
#define DEACTIVATE_CHRDEV_IOCTL_CMD 1

struct activation_ioctl_args {
	const char* data;
	size_t datalen;
};

static void __do_chrdev(const char* path, const char* passwd, const char* chrdev_path, int iocmd) {
	int fd = open(chrdev_path, 0);
	if(fd < 0) {
		perror("open");
		return;
	}

	char *str = concat(path, passwd);

	struct activation_ioctl_args act;
	act.data = str;
	act.datalen = strlen(str) + 1;
	
	int err = ioctl(fd, iocmd, &act);
	if(err < 0) {
		perror("ioctl");
	}

	close(fd);
	free(str);
}

static void activate_chrdev(const char* path, const char* passwd, const char* chrdev_path) {
	__do_chrdev(path, passwd, chrdev_path, ACTIVATE_CHRDEV_IOCTL_CMD);
}

static void deactivate_chrdev(const char* path, const char* passwd, const char* chrdev_path) {
	__do_chrdev(path, passwd, chrdev_path, DEACTIVATE_CHRDEV_IOCTL_CMD);
}

static void do_basic_checks_on(const char* prog, const char* path, const char* passwd) {
	if(passwd == NULL) {
		print_help(prog, "a password is required (see opt \"-p\")");
		exit(EXIT_FAILURE);
	}

	if(path == NULL) {
		print_help(prog, "a device (posix node) or regular file path is required (see opt \"-f\")");
		exit(EXIT_FAILURE);
	}

	if(strchr(passwd, '\r') != NULL) {
		print_help(prog, "passwords cannot contain a carriage return (aka \"\\r\")");
		exit(EXIT_FAILURE);
	}

	if(strchr(path, '\r') != NULL) {
		print_help(prog, "filepaths cannot contain a carriage return (aka \"\\r\")");
		exit(EXIT_FAILURE);
	}
}

typedef void (*mgmt_fpt)(const char*, const char*, const char*);

static mgmt_fpt get_mgmt_fn(const char* chrdev, bool activate) {
	if(chrdev == NULL && activate) {
		return activate_sysfs;
	} else if (chrdev == NULL && !activate) {
		return deactivate_sysfs;
	} else if (chrdev != NULL && activate) {
		return activate_chrdev;
	} else {
		return deactivate_chrdev;
	}
}

int main(int argc, char** argv){
	int ch;

	char *chrdev = NULL;
	char *filepath = NULL;
	char *passwd = NULL;
	bool need_to_activate = true;

	while((ch=getopt(argc, argv, "c:adf:p:hs")) != -1) {
		switch(ch) {
			case 'c':
				chrdev = optarg;
				break;
			case 's':
				chrdev = NULL;
				break;
			case 'a':
				need_to_activate = true;
				break;
			case 'd':
				need_to_activate = false;
				break;
			case 'f':
				filepath = optarg;
				break;
			case 'p':
				passwd = optarg;
				break;
			case 'h':
				print_help(argv[0], NULL);
				exit(EXIT_SUCCESS);
				break;
		}
	}

	do_basic_checks_on(argv[0], filepath, passwd);
	mgmt_fpt fn = get_mgmt_fn(chrdev, need_to_activate);

	fn(filepath, passwd, chrdev);

	exit(EXIT_SUCCESS);
}

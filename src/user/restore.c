#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>

static char *snapblocks_path = NULL;
static char *device_path = NULL;
static uint64_t restore_only_blknum = 0;
static bool restore_all = true;
static bool ask = true;

static void print_help(const char* prog, const char* msg) {
	if(msg) {
		fprintf(stderr, "args-error: %s\n", msg);
	}

	printf("usage: %s [-h] <-s snapblocks_path> <-f device path> [-n blknum] [-a or -o] [-p or -c]\n", prog);
	puts(" -h: prints this help");
	puts(" -s: specify the snapblocks path (mandatory)");
	puts(" -f: specify the block device or regular image path (mandatory)");
	puts(" -n: specify exactly one block number to restore (not mandatory)");
	puts(" -a: restore every block I find in snapblocks (not mandatory, default)");
	puts(" -o: dont restore every block I find in snapblocks (not mandatory)");
	puts(" -p: ask before each block restore (not mandatory, default)");
	puts(" -c: dont ask for each block I restore (not mandatory)");
}

static uint64_t to_u64(const char* arg) {
	errno = 0;
	uint64_t u = strtoull(arg, NULL, 10);
	if(errno != 0) {
		puts("unable to convert number to uint64_t");
		exit(EXIT_FAILURE);
	}

	return u;
}

#define SNAPBLOCK_MAGIC 0x5ade5aad5abe5aef

enum snapblock_payload_type : uint64_t {
	SNAPBLOCK_PAYLOAD_TYPE_RAW,
};

static const char* payload_type_to_str(enum snapblock_payload_type type) {
	if(type == SNAPBLOCK_PAYLOAD_TYPE_RAW) {
		return "raw fs blocks";
	}
	
	return "(unknown)";
}

struct snapblock_file_hdr {
	uint64_t magic;
	uint64_t blknr;
	uint64_t payldsiz;
	enum snapblock_payload_type payld_type;
	uint64_t payld_off;
} __attribute__((__packed__));

static void __do_restore_rawblocks(int snaps_fd, int device_fd, const struct snapblock_file_hdr *mhdr) {
	lseek(snaps_fd, mhdr->payld_off - sizeof(struct snapblock_file_hdr), SEEK_CUR);

	size_t nbytes = sizeof(uint8_t) * mhdr->payldsiz;
	uint8_t *buf = malloc(nbytes);

	if(buf == NULL) {
		lseek(snaps_fd, mhdr->payldsiz, SEEK_CUR);
		return;
	}

	ssize_t rerr = read(snaps_fd, buf, nbytes);

	if(rerr < 0) {
		perror("read");
		lseek(snaps_fd, mhdr->payldsiz, SEEK_CUR);
		free(buf);
		return;
	}

	if(rerr != (ssize_t) nbytes) {
		printf("unexpected reading error: could not read %ld bytes\n", nbytes);
		exit(EXIT_FAILURE);
	}

	lseek(device_fd, mhdr->blknr * nbytes, SEEK_SET);

	ssize_t werr = write(device_fd, buf, nbytes);

	if(werr < 0) {
		perror("write");
	}

	if(werr != (ssize_t) nbytes) {
		printf("unexpected writing error: could not write %ld bytes\n", nbytes);
		exit(EXIT_FAILURE);
	}

	free(buf);
}

#define seek_nexthdr(fd, hdr) \
	lseek(fd, (hdr).payld_off + (hdr).payldsiz - sizeof(struct snapblock_file_hdr) , SEEK_CUR);

static void restore_by_type(int snaps_fd, int device_fd, const struct snapblock_file_hdr* mhdr) {
	if(mhdr->payld_type == SNAPBLOCK_PAYLOAD_TYPE_RAW) {
		__do_restore_rawblocks(snaps_fd, device_fd, mhdr);
	} else {
		seek_nexthdr(snaps_fd, *mhdr);
	}
}

static void do_restore() {
	int snaps_fd = open(snapblocks_path, O_RDONLY);
	if(snaps_fd < 0) {
		fprintf(stderr, "open(%s): %s\n", snapblocks_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	int device_fd = open(device_path, O_WRONLY);
	if(device_fd < 0) {
		fprintf(stderr, "open(%s): %s\n", device_path, strerror(errno));
		close(snaps_fd);
		exit(EXIT_FAILURE);
	}
	
	struct snapblock_file_hdr hdrbuf;
	ssize_t hdrbufsize = sizeof(struct snapblock_file_hdr);
	ssize_t readerr;

	while((readerr = read(snaps_fd, &hdrbuf, hdrbufsize)) == hdrbufsize) {
		if(hdrbuf.magic != SNAPBLOCK_MAGIC) {
			puts("invalid magic number for snapblock header");
			puts("aborting");
			break;
		}

		if(!restore_all && restore_only_blknum != hdrbuf.blknr) {
			seek_nexthdr(snaps_fd, hdrbuf); 
			continue;
		}

		puts("+--------snapblock header-----------+");
		printf(" * block number: %ld\n", hdrbuf.blknr);
		printf(" * payload size: %ld\n", hdrbuf.payldsiz);
		printf(" * payload type: %s\n", payload_type_to_str(hdrbuf.payld_type));
		printf(" * payload offset at: %ld\n", hdrbuf.payld_off);
		puts("+-----------------------------------+");

		if(ask) {
			printf(" >>> would you like to restore this snapblock [yes/no]? ");

			char ans[10];
			memset(ans, 0, 10);
			fgets(ans, 10, stdin);

			if(strcmp(ans, "yes\n")) {
				puts(" --- skipping\n");
				seek_nexthdr(snaps_fd, hdrbuf);
				continue;
			}
		}

		puts(" !!! restoring...\n");
		restore_by_type(snaps_fd, device_fd, &hdrbuf);

		if(!restore_all && restore_only_blknum == hdrbuf.blknr) {
			break;
		}
	}

	if(readerr < 0) {
		perror("read");
	}

	close(snaps_fd);
	close(device_fd);
}

int main(int argc, char** argv) {
	int ch;
	while((ch = getopt(argc, argv, "hs:f:n:oapc")) != -1) {
		switch(ch) {
			case 'h':
				print_help(argv[0], NULL);
				exit(EXIT_SUCCESS);
				break;
			case 's':
				snapblocks_path = optarg;
				break;
			case 'f':
				device_path = optarg;
				break;
			case 'n':
				restore_only_blknum = to_u64(optarg);
				restore_all = false;
				break;
			case 'o':
				restore_all = false;
				break;
			case 'a':
				restore_all = true;
				break;
			case 'p':
				ask = true;
				break;
			case 'c':
				ask = false;
				break;
		}
	}

	if(snapblocks_path == NULL || device_path == NULL) {
		print_help(argv[0], "either snapblocks or device path not specified");
		exit(EXIT_FAILURE);
	}

	do_restore();

	exit(EXIT_SUCCESS);
}

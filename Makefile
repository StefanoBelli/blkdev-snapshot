all: kernel user

clean: kernel-src-clean user-src-clean

kernel:
	make -C src/kernel

user:
	make -C src/user

kernel-src-clean:
	make -C src/kernel clean

user-src-clean:
	make -C src/user clean

module-mount:
	make -C src/kernel mount

module-umount:
	make -C src/kernel umount

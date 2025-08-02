all:
	make -C kernel-module-src

clean:
	make -C kernel-module-src clean

module-mount:
	make -C kernel-module-src mount

module-umount:
	make -C kernel-module-src umount
modname=blkdev-snapshot
obj-m := $(modname).o
$(modname)-objs += main.o activation.o
ccflags-y += -Wall -W -Wextra -Wshadow -Wno-unused-parameter -O2 -I$(src)/include

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules
	
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean

mount:
	insmod $(modname).ko

umount:
	rmmod $(modname)

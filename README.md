# blkdev-snapshot
AOS project a.y. 2024/2025

## Index
1. [Introduction](#introduction)
2. [How to use it](#how-to-use-it)
3. [Module overview](#module-overview)

## Introduction
This is a Linux kernel module that allow the users to easily recover their data (e.g. mistakes) before the mount operation of a critical filesystem of interest.

Development of the module focused on various aspects:
 * resource usage
 * performance
 * extensibility
 * security
 * compatibility

## How to use it

### Build
To build the kernel module and the user programs, just run make from the root of the project:
~~~
 $ make
~~~
You will find the following executables: ```src/user/blkdev-restore```, ```src/user/blkdev-activation```.

And the loadable kernel module: ```src/kernel/blkdev-snapshot.ko```

### Loading the kernel module
To load the kernel module, run from the root of the project:
~~~
 $ make PASSWD=your-passwd module-mount
~~~

If password is not provided (make forwards to insmod) then module init fails with ENODATA.

### Unloading the kernel module
To unload the kernel module, run from the root of the project:
~~~
 $ make module-umount
~~~

### Activating/deactivating the snapshot
The module provides either sysfs or chrdev ioctl as interfaces to it, 
it auto-chooses one of them when compiling by checking 
for preprocessor macro ```CONFIG_SYSFS``` which is most-likely defined 
(maybe except on embedded or specific-purpose system).

#### The sysfs-way of doing this
To activate or deactivate the snapshot for a particular device or regular image file, you have two options:

 * Using ```echo``` directly:
~~~
# echo -ne '/dev/sda1\rpasswd\0' > /sys/module/blkdev_snapshot/activate_snapshot
~~~
or
~~~
# echo -ne 'path/to/regfile.img\rpasswd\0' > /sys/module/blkdev_snapshot/activate_snapshot
~~~
or
~~~
# echo -ne '/dev/loop0\rpasswd\0' > /sys/module/blkdev_snapshot/activate_snapshot
~~~

Note that the kernel module will check if the passed path is related to a regular file, block device or
block device which uses the loop driver, in this last case, module will extract the backing (regular) 
file image of it. The \r is used by the module to distinguish dev_name and passwd.

 * Using ```blkdev-activation``` tool:
After the build, from the root project source tree
~~~
# ./src/user/blkdev-activation -a -f /dev/sda1 -p passwd
~~~
or
~~~
# ./src/user/blkdev-activation -a -f path/to/regfile.img -p passwd
~~~
or
~~~
# ./src/user/blkdev-activation -a -f /dev/loop0 -p passwd
~~~

##### Deactivating
Same thing:
~~~
# echo -ne '/dev/sda1\rpasswd\0' > /sys/module/blkdev_snapshot/deactivate_snapshot
~~~

or

~~~
# ./src/user/blkdev-activation -d -f /dev/sda1 -p passwd
~~~

#### The chrdev-way of doing this
You will need to check for dmesg for the assigned major number and do mknod:

~~~
# mknod 240 1 c bdactchrdev
~~~

Then you can use the blkdev-activation tool

~~~
# ./src/user/blkdev-activation -c bdactchrdev -a -f path/to/regfile.img -p passwd
~~~

##### Deactivating

~~~
# ./src/user/blkdev-activation -c bdactchrdev -d -f path/to/regfile.img -p passwd
~~~

Note that it is possible to try to request another major number by specifying it 
explicitly using the C preprocessor macro ```ACTDEVREQMAJ```. If kernel cannot satisfy
the ```register_chrdev``` request using that major number (or even by not specifying it)
then module is taken down (init failure). Note that if chrdev support is needed (no sysfs)
then you must also specify the password via the ```ACTPASSWD``` C preprocessor macro.

This requires Makefile changes and it is not supported by default since sysfs is widely
enabled and required for every Linux general purpose system to work correctly.

### Work on your filesystem...

After activating the snapshot mount your fs and use it. 

If fs was already mounted prior the activation the module will not catch any write, 
so you must ```umount``` and ```mount``` again.

Mounting increases the "```n_currently_mounted```" counter for the epoch (but not when mount 
is used with --move opt), umount makes it decrease. When it reaches 0 then another "epoch" 
will be created once a new mount is done.

Each time a new "epoch" is created (new "first" mount) then when a write is detected we create
a subdirectory in /snapshot which is in the form *orignal dev name*-*first mount timestamp*, and
inside of it, the *snapblocks* file will be created. 

Both the /snapshot directory and its subdirs will
be created in "lazy" mode (when it is necessary, i.e. a concrete write is catched by the kprobes).

### Restoring snapshot

To restore the snapshot just use the ```blkdev-restore``` tool in ```src/user``` from the root project:

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f path/to/image
~~~

or

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f /dev/sda2
~~~

or

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f /dev/loop0
~~~

Anyway, this program will walk through every snapshot in the *snapblocks* file and ask you to confirm 
explicitly with a "yes" to restore the encountered block.

To avoid this behaviour, use the option *-c*: every block will be restored without asking.

If you want to restore only one particular block, use the option *-n <nr_block>* to do it.

Options can be mixed.

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f /dev/loop0 -n 10 -c
~~~

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f /dev/loop0 -c
~~~

~~~
# ./src/user/blkdev-restore -s /snapshot/image-date_of_mount/snapblocks -f /dev/loop0 -n 20
~~~

Anyway, for both user tools help is available via the *-h* option.

## Module overview
WIP



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
To build the kernel module and the user programs, just run make from the root of the project source tree:
~~~
 $ make
~~~
You will find the following executables: ```src/user/blkdev-restore```, ```src/user/blkdev-activation```.

And the loadable kernel module: ```src/kernel/blkdev-snapshot.ko```

### Loading the kernel module
To load the kernel module, run from the root of the project source tree:
~~~
 $ make PASSWD=your-passwd module-mount
~~~

If password is not provided (make forwards to insmod) then module init fails with ENODATA.

### Unloading the kernel module
To unload the kernel module, run from the root of the project source tree:
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

To restore the snapshot just use the ```blkdev-restore``` tool in ```src/user``` from the root project source tree:

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

### Activating, deactivating and password in-kernel-memory store

Starting from the user interface to ```activate_snapshot``` and ```deactiate_snapshot```, 
as already said before, this module supports both sysfs and chrdev ioctl. Sysfs is the favourite one.

In both cases, a password will be needed to use the aforementioned functions and the password is stored using
SHA-256 + salt, in kernel memory. Did this thanks to crypto_shash API of the kernel.

If sysfs is used, then no password will be stored in the .ko file (variables set via insmod and all the sysfs stuff),
otherwise, it will be stored in cleartext in the .ko file (ensure proper protection of the .ko file), but when the
module is loaded, in kernel memory, the password will be hashed with salt anyway and the ro memory that contains the
passwd will be cleared as soon as possible (CR0.WP disabling/enabling trick). 

Code related to these parts is in ```src/kernel/passwd.c``` and ```src/kernel/activation.c```.

### Device management

To store infos about devices for which the user requested snapshot service activation a rhashtable with automatic shrinking is used.
Asynchronous nature of the project makes the reliance on RCU almost mandatory.
As already explained above, both the activating and the deactivating funcion will accept both regular and block device files. If a
regular file is detected, then it is treated as loop device (```struct loop_object``` rhashtable node), otherwise, if a block device
is detected then it is treated as either as a real block device (registered in ```struct blkdev_object``` rhashtable node) 
or a loop bdev (registration goes to ```struct loop_object```), that is, by getting the backing file name from bdev private data 
if its major number corresponds to the loop device driver one.
An attempt has been made to made the code compatible across many linux versions (see ```get_loop_backing_file``` in ```src/kernel/devices.c```).
The key for the rhashtable for loop devices is a full path to the image on host fs, while for the block devices rhashtable, the key is the dev_t.
For both of them, the "value" is represented by a ```struct object_data``` which is a representation for the registered device 
(it contains the ptr to the current ```struct epoch```).
Both rhashtable exist independently but client code doesn't know it and either one is used while querying or modifying (e.g. by determining the type of bdev).

The "outer" ```activate_snapshot``` and ```deactivate_snapshot``` call ```register_device``` and ```unregister_device``` which will 
call the init object data or cleanup object data functions. The init object data will init the spinlocks, zero the current epoch ptr, 
copy the original passed dev_name and init the ordered wq for the device (which is valid across all epochs) in which snapshot deferred work and 
epoch cleanup deferred work will be put, considering its ordering property. When cleanup object data function is called, particular care must be taken, since
it will be called both in process and atomic context and will be called when module is unloaded or when user requests snapshot service deactivation.
Deferred work for ```struct object_data``` complete deactivation and kfreeing is needed. This time, this work goes on a system wq, 
and the work gets all ptrs to do flush_workqueue, destroy and epoch freeing (locks gurantee that there will not be a double reference putting 
or double kfrees, etc... when ptrs are copied, they are cleared from the "public" structure so the code that follows understands it and ignores any 
freeing-like operations) since epoch can be cleaned both by the user that suddenly decides to deactivate the snapshot service or by a real unmounting 
that causes the epoch to terminate. On module exit, we may also need to wait for every of this cleanup object data work to terminate 
(by storing in a linked list all their work_struct), since otherwise code of unloaded module may be executed causing page fault.

An epoch is composed of a mount counter, timestamp of first mount, a struct path* and a struct lru_ng*. 
The struct path* will be initialized by the first deferred snapshot work to the /snapshot/image-<timestamp>/ directory, 
path_getted and handed over to all the successive work. Same for the LRU cache.

Code related to this part is in ```src/kernel/include/devices.h```, ```src/kernel/devices.c```, ```src/kernel/include/get-loop-backing-file.h```

### Mount detection

Mounts are detected by installing ```kretprobes``` in ```do_move_mount```, ```path_mount``` and ```path_umount```.

This is because one of the aim in development was to ensure future extensiblity (e.g. other fs support, not to rely on any 
fs-specific way of mounting) and compatibility.

In fact, from kernel 5.2 another way to do mounts was introduced (the newer ```fsconfig```, ..., ```do_move_mount``` way) and userspace tool
like mount started to support it later on, the probe is installed only on ```do_move_mount```, which will let us able to determine if device
is mounted correctly or not.

The module is able to detect both the new way (```do_move_mount```) 
and the old way (```path_mount```) of doing mounts, it just depends on userspace tool, most likely only 
```do_move_mounts``` will be used nowadays.

Probes installed on both of them do the same thing but parse incoming data in different way (to determine if the mount is 
really new or not, e.g. --move) either one is used (again, depends on userspace tools), but, at the end of the day, 
both of them make the ```n_currently_mounted``` counter to increase (if snapshot service is activated for the particular device).

Since kernel 5.9 ```path_umount``` is available and can be used to easily detect umounts.

When an epoch event (umount- or mount- ing - that is - increasing or not the ```n_currently_mounted``` counter) happens, then module determines if
for the device the snapshot service is activated, and, if so, it increases or decreases the epoch counter.

The containing ```struct object_data``` for ```struct epoch``` has a ```general_lock``` which is taken to increase or decrease the counter.

If the event was a umount and counter reaches 0, then, since we have the ```general_lock``` we can safely queue the work on the 
ordered wq for the snapshot service for the device for current epoch cleanup (this is because other works are in flight and rely on epoch data, 
since the wq is ordered...). If the event was a mount and no epoch is alive, then ```kzalloc``` in atomic context will allocate a 
new ```struct epoch``` which will be used by all the following snapshot deferred work in the ordered wq. 
The new ```struct epoch``` is initialized by incrementing its counter (0 to 1) and setting "now" date (the first detected mount date). 
Please note that this is kernel-provided date in UTC time.

Code related to this part is in ```src/kernel/mounts.c```.

### Snapshot

The snapshot is concretely made from a FS-independent part. Symbols are exported for any other part of the kernel to use.

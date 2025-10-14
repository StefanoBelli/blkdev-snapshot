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

 * 

## Module overview
WIP


#!/bin/bash

USERBIN=$PWD/../src/user/
SFFSBIN=$PWD/SINGLEFILE-FS
DEMOIMG=demo.img
MNTPOINT=mnt
PRIOR_MD5SUM=""
MODULE_PASSWD=ciao

do_mount() {
	sudo mount -o loop -t singlefilefs $DEMOIMG $MNTPOINT
}

do_umount() {
	sudo umount $MNTPOINT
}

the_file_write() {
	$SFFSBIN/user/user $MNTPOINT/the-file $1 $2
}

prepare_demo() {
	numblks=10
	if [ $# -ge 1 ]; then
		numblks=$1
	fi

	echo "num of blocks for test file: $numblks"

	cd ..
	sudo make PASSWD=$MODULE_PASSWD module-mount 2>/dev/null >>/dev/null
	cd demo

	cd $SFFSBIN
	sudo make load-FS-driver 2>/dev/null >>/dev/null
	cd ..

	if [ -f $DEMOIMG ]; then
		rm -v $DEMOIMG
	fi

	dd if=/dev/zero of=$DEMOIMG bs=4096 count=100

	$SFFSBIN/singlefilemakefs $DEMOIMG

	if [ ! -d $MNTPOINT ]; then
		mkdir -v $MNTPOINT
	fi

	do_mount

	if [ $numblks -ne 0 ]; then
		the_file_write $(python3 -c "print(\"a\" * $numblks * 4096)") 0
	fi

	do_umount

	PRIOR_MD5SUM=$(md5sum $DEMOIMG | awk '{ print $1 }')
}

activate_device() {
	sudo $USERBIN/blkdev-activation -a -f $DEMOIMG -p $MODULE_PASSWD
	sudo rm -rv /snapshot
}

deactivate_device() {
	sudo $USERBIN/blkdev-activation -d -f $DEMOIMG -p $MODULE_PASSWD
}

check_results() {
	INTERM_MD5SUM=$(md5sum $DEMOIMG | awk '{ print $1 }')

	sudo $USERBIN/blkdev-restore -c -s /snapshot/$(sudo ls /snapshot)/snapblocks -f $DEMOIMG
	NOW_MD5SUM=$(md5sum $DEMOIMG | awk '{ print $1 }')
	echo ""
	
	rv=0
	if [[ $PRIOR_MD5SUM == $NOW_MD5SUM ]]; then
		echo "+++ RESTORE SUCCESSFUL +++"
	else
		echo "--- RESTORE FAILED ---"
		rv=1
	fi

	echo ""
	echo pre-mountop hash: $PRIOR_MD5SUM
	echo after-writes hash: $INTERM_MD5SUM
	echo post-restore hash: $NOW_MD5SUM
	echo ""

	return $rv
}

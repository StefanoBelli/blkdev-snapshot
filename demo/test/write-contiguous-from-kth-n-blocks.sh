#!/bin/bash

source utils.sh

prepare_demo
activate_device
do_mount
the_file_write ciaociaociao 24576
the_file_write ciaociaociao 28672
the_file_write ciaociaociao 24578
the_file_write ciaociaociao 20480
the_file_write ciaociaociao 20470
do_umount
deactivate_device
check_results && exit 0 || exit 1


#!/bin/bash

source utils.sh

prepare_demo
activate_device
do_mount
the_file_write ciaociaociao 32768
the_file_write ciaociaociao 0
the_file_write ciaociaociao 8192
the_file_write ciaociaociao 800
the_file_write ciaociaociao 20488
the_file_write ciaociaociao 40960
the_file_write ciaociaociao 20480
do_umount
deactivate_device
check_results && exit 0 || exit 1

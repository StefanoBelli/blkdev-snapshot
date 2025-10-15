#!/bin/bash

source utils.sh

prepare_demo 1
activate_device
do_mount
the_file_write ciaociaociao 4093
do_umount
deactivate_device
check_results && exit 0 || exit 1

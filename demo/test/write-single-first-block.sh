#!/bin/bash

source utils.sh

prepare_demo
activate_device
do_mount
the_file_write ciaociaociao 0
the_file_write provaprova 100
do_umount
deactivate_device
check_results && exit 0 || exit 1

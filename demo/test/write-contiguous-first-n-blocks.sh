#!/bin/bash

source utils.sh

prepare_demo
activate_device
do_mount
the_file_write ciaociaociao 0
the_file_write provaprova 4097
the_file_write quickquickquickquick 8200
the_file_write olololololollolo 12300
do_umount
deactivate_device
check_results && exit 0 || exit 1

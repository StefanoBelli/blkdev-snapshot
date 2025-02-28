#!/bin/bash

DISTRO=$(cat /etc/os-release | grep ^ID= | tr '=' '\n' | tail -1)

ln -sf $DISTRO\_c_cpp_properties.json .vscode/c_cpp_properties.json && \
    echo "set lkm devel vscode config for $DISTRO: ok"

GCC_TARGET=$(gcc -v 2>&1 | grep 'Target' | awk '{ print $2 }')
GCC_DUMPVER=$(gcc -dumpversion)
LINUX_UNAMER=$(uname -r)

sed -i "s/__template_GCC_TARGET/$GCC_TARGET/g" .vscode/c_cpp_properties.json
sed -i "s/__template_GCC_DUMPVER/$GCC_DUMPVER/g" .vscode/c_cpp_properties.json
sed -i "s/__template_LINUX_UNAMER/$LINUX_UNAMER/g" .vscode/c_cpp_properties.json

echo " -- gcc target: $GCC_TARGET"
echo " -- gcc version: $GCC_DUMPVER"
echo " -- linux version: $LINUX_UNAMER"
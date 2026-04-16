#!/bin/bash
if [ -z "${LLVM_DIR}" ]; then
    echo "Enter the path to your LLVM bin directory (e.g. /usr/lib/llvm-19/bin): "
    read path

    export LLVM_DIR="$path"

    echo "If you want to persistently add LLVM_DIR to your path"
    echo "modify the .bashrc script"
fi


mkdir build
cd build
cmake -DLT_LLVM_INSTALL_DIR=$LLVM_DIR ../

make
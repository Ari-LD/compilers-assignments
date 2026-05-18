#!/bin/bash

usage() {
    echo "usage: $0 -t <test_dir> -p <plugin_path>"
    echo "  -t: Directory containing .cpp test files"
    echo "  -p: Plugin path"
    exit 1
}

TEST_DIR=""
PLUGIN_PATH=""

while getopts "t:p:" opt; do
    case "$opt" in
        t) TEST_DIR=$OPTARG ;;
        p) PLUGIN_PATH=$OPTARG ;;
        *) usage ;;
    esac
done

shift $((OPTIND-1))


if [ -z "$TEST_DIR" ] || [ -z "$PLUGIN_PATH" ]; then
    echo "Error: Missing parameters."
    usage
fi

if [ ! -d "$TEST_DIR" ]; then echo "Error: test directory not found."; exit 1; fi
if [ ! -f "$PLUGIN_PATH" ]; then echo "Error: Plugin not found."; exit 1; fi

LL_DIR="$TEST_DIR/ll"
OPT_DIR="$TEST_DIR/optimized"
mkdir -p "$LL_DIR" "$OPT_DIR"

echo "---------------------------------"

for cpp_file in "$TEST_DIR"/*.cpp; do
    [ -e "$cpp_file" ] || continue
    filename=$(basename "$cpp_file" .cpp)
    
    echo "Processing: $filename"

    if ! clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$cpp_file" -o "$LL_DIR/$filename.ll"; then
        echo "   [ERROR] failed compilation for file: $filename"
        continue
    fi

    opt -load-pass-plugin "$PLUGIN_PATH" -passes="mem2reg" "$LL_DIR/$filename.ll" -S -o "$LL_DIR/${filename}.ll"
    opt  -load-pass-plugin "$PLUGIN_PATH" -passes="LI-CM" "$LL_DIR/$filename.ll" -S -o "$OPT_DIR/${filename}_opt.ll"

    echo "   [OK] Completed."

done
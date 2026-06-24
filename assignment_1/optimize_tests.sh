#!/bin/bash

ALLOWED_PASSES=("algebraic-identity" "strength-reduction" "multi-instruction")

usage() {
    echo "usage: $0 -t <test_dir> -p <plugin_path> <pass1> <pass2> ..."
    echo "  -t: Directory containing .cpp test files"
    echo "  -p: Plugin path"
    echo "  Allowed optimizations: ${ALLOWED_PASSES[*]}"
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

# 1. Normalizzazione: uniamo tutto e sostituiamo eventuali virgole con spazi
# Questo permette all'utente di scrivere sia "pass1 pass2" che "pass1,pass2"
INPUT_ARGS=$(echo "$*" | tr ',' ' ')

# 2. Validazione dei passaggi
VALIDATED_PASSES=()
for arg in $INPUT_ARGS; do
    is_valid=false
    for allowed in "${ALLOWED_PASSES[@]}"; do
        if [ "$arg" == "$allowed" ]; then
            is_valid=true
            VALIDATED_PASSES+=("$arg")
            break
        fi
    done
    
    if [ "$is_valid" = false ]; then
        echo "Error: pass '$arg' is not recognized."
        exit 1
    fi
done

# Controllo se è stato inserito almeno un passaggio
if [ ${#VALIDATED_PASSES[@]} -eq 0 ]; then
    echo "Error: No optimization specified."
    usage
fi

# 3. Creazione della stringa per opt (unisce con le virgole)
PASS_STRING=$(IFS=,; echo "${VALIDATED_PASSES[*]}")

# Controlli percorsi
if [ -z "$TEST_DIR" ] || [ -z "$PLUGIN_PATH" ]; then
    echo "Error: Missing parameters."
    usage
fi

if [ ! -d "$TEST_DIR" ]; then echo "Error: test directory not found."; exit 1; fi
if [ ! -f "$PLUGIN_PATH" ]; then echo "Error: Plugin not found."; exit 1; fi

LL_DIR="$TEST_DIR/ll"
OPT_DIR="$TEST_DIR/optimized"
mkdir -p "$LL_DIR" "$OPT_DIR"

echo "Passes to execute: $PASS_STRING"
echo "---------------------------------"

for cpp_file in "$TEST_DIR"/*.cpp; do
    [ -e "$cpp_file" ] || continue
    filename=$(basename "$cpp_file" .cpp)
    
    echo "Processing: $filename"

    # Compilazione
    if ! clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$cpp_file" -o "$LL_DIR/$filename.ll"; then
        echo "   [ERROR] failed compilation for file: $filename"
        continue
    fi

    opt -load-pass-plugin "$PLUGIN_PATH" -passes="mem2reg" "$LL_DIR/$filename.ll" -S -o "$LL_DIR/${filename}.ll"
    opt -load-pass-plugin "$PLUGIN_PATH" -passes="$PASS_STRING" "$LL_DIR/$filename.ll" -S -o "$OPT_DIR/${filename}_opt.ll"

    echo "   [OK] Completed."

done
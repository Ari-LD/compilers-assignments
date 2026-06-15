# Fourth Assignment
## Purpose
Create an LLVM pass that implements a **Loop Fusion** optimization
## How To Use
### Prepare the environment
- LLVM-19 and clang++ 19 are required (in particular the commands opt and clang++ must be available)
- To prepare the environment, you can run the script init.sh which will ask you to insert the path to your llvm installation
- Alternatively, you can manually prepare the environment and compile the plugin using
```bash
export LLVM_DIR=path/to/llvm
mkdir build
cd build
cmake -DLT_LLVM_INSTALL_DIR=$LLVM_DIR ..
make
 ```

### Running the optimizer
- To automatically run the optimizer for all .cpp tests, it's possible to use optimize_test.sh:
```bash
./optimize_test.sh -t <test_dir_path> -p <plugin_path>
```

- Alternatively, it is possibile to run the plugin for a specific .ll file using:
```bash
opt -load-pass-plugin <plugin_path> -passes="LF" <input_file> -S -o <output_file>
```
---

## Loop Fusion Explanation

This pass allows us to merge (when possible) iterations from multiple loops in a single one, reducing loop overhead and taking advantage of cache locality.

---

### Example

```c++
  /* The two outer loops can be merged
  *  once we merge them, we proceed to the next nest level
  *  and merge the two nested loops */
  for (int i = 0; i < n; i++) {
    for(int j = 0; j < m; j++){
      int c = A[i][j] + 1;
    }
  }

  for (int i = 0; i < n; i++) {
    for(int j = 0; j < m; j++){
      int d = A[i][j] * 2;
    }
  }
```
Which becomes

```c++
  // better reuse of data, less loop checks and increments
  for (int i = 0; i < n; i++) {
    for(int j = 0; j < m; j++){
      int c = A[i][j] + 1;
      int d = A[i][j] * 2;
    }
  }
```

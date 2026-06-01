# Third Assignment
## Purpose
Create an LLVM pass that implements a **Loop Invariant Code Motion** or **LICM** optimization
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
opt -load-pass-plugin <plugin_path> -passes="LI-CM" <input_file> -S -o <output_file>
```
---

## LICM Explanation

This pass allows us to remove redundant operation from inside a loop, in particular we put instructions that need to be executed only once ***before*** the loop, saving computation time.

---

### Example

```c++
int a=3;
int b=4;
int e=0;
int c, d, f;

do{
    c = a+b; //this instruction can be moved before the loop

    d = c+1; // this instruction can be moved before the loop
                    // (we need to make sure "c=a+b" is put first)
        
    f = i+3; // this instruction is not loop invariant, so it cannot be moved
    
    if(f>10){
    /* this instruction is loop invariant, but it cannot be moved.
    In fact, we could overwrite "e" even if the condition is false.
    In the specific case "e" is not used after the loop, the optimization 
    could still be considered valid
    */
        e=3; 
    }
    i++;
}while(i<n);
```

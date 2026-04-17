# First Assignment
## Purpose
Implement three LLVM passes that achieve the following optimizations:
1. ***Algebraic Identity***
2. ***Strength Reduction***
3. ***Multi-Instruction Optimization***
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
./optimize_test.sh -t <test_dir_path> -p <plugin_path> <pass1> <pass2> ...
```
The possible passes are **algebraic-identity, strength-reduction** and **multi-instruction**

- Alternatively, it is possibile to run the plugin for a specific .ll file using:
```bash
opt -load-pass-plugin <plugin_path> -passes=<passes_to_execute> <input_file> -S -o <output_file>
```
---

### Generate the documentation
Doxygen is required to create the documentation, run the following command inside the "assignment_1" directory:
```bash
doxygen
```
this will create a "doc" directory with the various files in html and latex

## Code Explanation

This is an **LLVM compiler plugin** that implements two optimization passes operating on LLVM's Intermediate Representation (IR).

---

### 1. Algebraic Identity

Eliminates useless operations based on algebraic identity rules:

#### **Single costant identities**

In these cases, if a constant operand has a specific value (such as 0 or 1), an entire instruction can be substituted a variable or constant.

*   **Zero addition:** `x + 0 = x`, `0 + x = x`.
    
*   **Zero subtraction:** `x - 0 = x`.

*   **Multiplication by 0:** `x * 0 = 0`, `0 * x = 0`.
    
*   **Multiplication by 1:** `x * 1 = x`, `1 * x = x`.
    
*   **Division by one:** `x / 1 = x`.
    
*   **Shift (Left, Logical Right, Arithmetic Right) by zero:** `x << 0 = x`, `x >> 0 = x`.
    
*   **Bitwise AND by -1 and 0:** `x & -1 = x`, `x & 0 = 0`.
    
*   **Bitwise OR/XOR by zero:** `x | 0 = x`, `x ^ 0 = x`.

*   **Modulo by 1:** `x % 1 = 0`.
    

#### **Identities with the same operands**

In these cases, an instruction can be replaced if the two operands are the same.

*   **Identical operands subtraction:** `x - x = 0`.
    
*   **Identical operands division:** `x / x = 1`.
    
*   **Identical operands in bitwise AND/OR:** `x & x = x`, `x | x = x`.
    
*   **Identical operands  XOR:** `x ^ x = 0`.

*   **Identical operands in modulo:** `x % x = 0`.

---

### 2. Strength Reduction

Replaces **expensive** operations (multiplications and divisions) with **cheaper** ones:

**Multiplication (`Mul`):**
| Case | Example | Replacement |
|---|---|---|
| Power of 2 | `x * 8` | `x << 3` |
| Power of 2 − 1 | `x * 7` | `(x << 3) - x` |
| Power of 2 + 1 | `x * 9` | `(x << 3) + x` |
| Multiplication by -1 | `x * (-1)` | `0 - x` |
| Sum of 2 powers | `x * 40` | `(x << 5) + (x << 3)` |
| Sub of 2 powers | `x * 56` | `(x << 6) - (x << 3)` | 

**Integer Division (`SDiv/Udiv`):**
- If the divisor is a power of 2 → arithmetic right shift (`AShr`)
- Example: `x / 4` → `x >> 2`

**Integer Signed Remainder (`SRem`):**
- if the constant is a power of 2 → shift right then left, if the result is different from x something was lost, by subtracting to the original variable you get the remainder
- Example: `x % 8` → `x - ((x >> k) << k)`

**Integer Unsigned Remainder (`URem`):**
- if the constant is a power of 2 → subtract 1 from the constant and do an (`And`)
- Example: `x % 4` → `x & 3`

---

### 3. Multi-Instruction
Instructions can be removed if their value can be obtained from previous instructions:

- `a = b+1, c = a-1 --> c = b`
- `a = b*3, c = a/3 --> c = b`
- `a = b & 5, c = a & 5 --> c = a`
- `a = b & 1, c = a & 2 --> c = 0`
- `a = b | 5, c = a | 5 --> c = a`
- `a = b ^ 5, c = a ^ 5 --> c = b`
- `a = b ^ 3, c = a ^ 7, d = c ^ 4 --> d = b`

---

### Shared Structure

Both passes follow the same three-level traversal:

```
run() → runOnFunction() → runOnBasicBlock()
```

They iterate over every **BasicBlock** in the function, and for each instruction check whether the optimization applies. The `++iter` is done **before** modifying the instruction to avoid iterator invalidation.

---

### Plugin Registration

At the bottom, the three passes are registered as LLVM passes under the names:
- `algebraic-identity`
- `strength-reduction`
- `multi-instruction` 


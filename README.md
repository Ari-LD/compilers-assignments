# Compiler Assignments
## Intro
*Assignments for the Compilers course exam, creating the middle-end of a compiler; we will use LLVM and the C++ programming language*

> LLVM (Low Level Virtual Machine) is a set of compiler and toolchain technologies that can be used to develop a frontend for any programming language and a backend for any instruction set architecture. LLVM is designed around a language-independent intermediate representation (IR) that serves as a portable, high-level assembly language that can be optimized with a variety of transformations over multiple passes.

- The Middle-end block (optimizer) operates on the same
IR produced by each Front-end and received as input by
each Back-end.
- To support a new language, you simply
write a new Front-end.
- To support a new target (ISA), you simply
write a new Back-end.

## Ingredients of Optimization
- Formulate an optimization problem
    - Identify optimization opportunities
        - Applicable to many programs
        - Impact significant parts of the program
        (loops/recursion)
        - Sufficiently efficient
- Representation
    - Must abstract the details relevant to the optimization
-  Analysis
    - Determine whether it is safe and desirable to apply a transformation
- Code transformation
    - Experimental validation (and the process is repeated)

## 👥 Contributors

* <a href="https://github.com/derosacarmine/"><img src="https://github.com/derosacarmine.png" width="35px" style="border-radius:50%; vertical-align:middle; margin-right:5px;"/></a> **Carmine De Rosa** — [![GitHub](https://img.shields.io/badge/GitHub-derosacarmine-181717?style=flat&logo=github)](https://github.com/derosacarmine/)
* <a href="https://github.com/GHManu"><img src="https://github.com/GHManu.png" width="35px" style="border-radius:50%; vertical-align:middle; margin-right:5px;"/></a> **Manuel Gherardi** — [![GitHub](https://img.shields.io/badge/GitHub-GHManu-181717?style=flat&logo=github)](https://github.com/GHManu)
* <a href="https://github.com/Ari-LD"><img src="https://github.com/Ari-LD.png" width="35px" style="border-radius:50%; vertical-align:middle; margin-right:5px;"/></a> **Arild Kuti** — [![GitHub](https://img.shields.io/badge/GitHub-Ari--LD-181717?style=flat&logo=github)](https://github.com/Ari-LD)

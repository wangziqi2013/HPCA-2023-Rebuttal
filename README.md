
HPCA 2023 Rebuttal Supplementary Files
======================================

We submit this anonymized repository as supplementary material to our HPCA-2023 rebuttal on 
multi-block compression (MBC) paper. 
This repo contains a reference implementation (and the implementation we use for evaluation) of 
the `malloc` library we proposed in our research paper.

The `malloc` library differs from regular `malloc` by placing small objects (from 1 byte to 512 bytes) 
of the same type on the same arena memory region. For each type, the library `mmap`s a chunk of memory
called an "arena", and uses a "free list" (i.e., a singly linked list) to manage free objects in the arena. 
Object allocation happens by simply removing the head object from the free list.
Object deallocation happens by inserting the object back to the head of the free list.

For completeness, this library also implements allocation and deallocation semantics larger than 512 bytes. 
However, these implementations are mostly irrelevant to our proposed approach. We added them nevertheless 
such that reviewers can compile the library and try it out on applications.

How It Helps Inter-Block Compression
------------------------------------

The source code contains logic for placing same-type objects on the same arena. 
At the application level, programmers can either explicitly include the library's header file, and call
`malloc_2d_typed_alloc()` with an explicitly assigned type ID, or call `malloc_2d_typed_alloc_implicit()`
with an implicitly generated type ID. In the latter case, the library takes the address of the call site as the 
type ID such that objects allocated at different call sites will be placed on different arenas.

When a new arena is created, the `malloc` library calculates the "step size" attribute of the arena, 
and notifies the OS of the attribute. In our implementation, since OS support is non-existent, we simply
inserted a call to the simulator such that the simulated LLC controller will compute set indices and tag addresses
differently as proposed in our paper (see line 54--60).

Theoretically speaking, there are also two potential use cases that can be explored as future work.
First, in static-typed languages, the compiler could assist assigning the type ID when `malloc` is 
called in the application because the compiler is aware of both `malloc` semantics and the static type 
of an object.
Second, programmers should be able to use `LD_PRELOAD` to override the existing `malloc` library and replace 
it with ours to support inter-block compression. Currently, our implementation can be compiled into a `.so` file
and can be `LD_PRELOAD`'ed into many applications without breaking them 
(e.g., try `LD_PRELOAD=./malloc_2d.so ls -al`; Due to time constraints we are still debugging it, so there can be 
occasional crashes). 

Build Instructions
------------------

Just type `make`, and the makefile will compile a shared object file `libmalloc_2d.so`. 

The source files can also be copied to an existing project directly and be used as a source-code 
level library. 

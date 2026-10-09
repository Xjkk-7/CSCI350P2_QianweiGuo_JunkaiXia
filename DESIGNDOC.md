Task 1: Kernel Level Threads (KLT)

kthread.h: Declared the four KLT APIs. proc.h: Added join bookkeeping and process lifecycle fields to coordinate thread cleanup, exec, and exit. proc.c: Implemented the four thread operations, updated thread allocation and cleanup, synchronized sbrk memory updates, and modified fork to duplicate only the calling thread and exit to terminate all threads before releasing process resources. exec.c: Added coordination to stop sibling threads before replacing the address space and release the exec reservation on failure. sysproc.c: Added syscall wrappers for the four thread APIs and updated sbrk and sleep handling. syscall.c: Registered the four thread system calls in the syscall table. defs.h: Declared the internal helper functions. 


Task 2: Synchronization Primitive Mutex

kthread.h: Defined MAX_MUTEXES as 64 and declared the four mutex APIs.
proc.h: Added TBLOCKED and a mutex structure containing its ID, state, and waiting-thread count.
proc.c: Implemented the four mutex operations using a spinlock-protected table, blocked waiting threads without busy waiting, and updated wakeup and termination paths to handle TBLOCKED.
sysproc.c: Added syscall wrappers to retrieve arguments and call the corresponding mutex functions.
syscall.c: Registered the four mutex system calls in the syscall table.

Task 2: Synchronization Primitive Mutex

kthread.h: Defined MAX_MUTEXES as 64 and declared the four mutex APIs.
proc.h: Added TBLOCKED and a mutex structure containing its ID, state, and waiting-thread count.
proc.c: Implemented the four mutex operations using a spinlock-protected table, blocked waiting threads without busy waiting, and updated wakeup and termination paths to handle TBLOCKED.
sysproc.c: Added syscall wrappers to retrieve arguments and call the corresponding mutex functions.
syscall.c: Registered the four mutex system calls in the syscall table.

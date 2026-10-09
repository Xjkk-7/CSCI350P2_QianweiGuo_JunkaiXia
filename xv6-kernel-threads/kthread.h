#ifndef XV6_PUBLIC_KTHREAD_H
#define XV6_PUBLIC_KTHREAD_H

// edited by: Qianwei Guo
// part 2 mutex definitions
#define NTHREAD			16
#define MAX_MUTEXES     64

int kthread_mutex_alloc(void);
int kthread_mutex_dealloc(int mutex_id);
int kthread_mutex_lock(int mutex_id);
int kthread_mutex_unlock(int mutex_id);

//edited by: Junkai Xia
// Declare the four kernel thread APIs.
int kthread_create(void* (*start_func)(), void* stack, int stack_size);
int kthread_id(void);
void kthread_exit(void);
int kthread_join(int thread_id);

#endif

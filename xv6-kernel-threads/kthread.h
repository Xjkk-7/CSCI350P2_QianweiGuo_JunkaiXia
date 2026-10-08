#ifndef XV6_PUBLIC_KTHREAD_H
#define XV6_PUBLIC_KTHREAD_H

#define NTHREAD			16

int kthread_create(void* (*start_func)(), void* stack, int stack_size);
int kthread_id(void);
void kthread_exit(void);
int kthread_join(int thread_id);

#endif

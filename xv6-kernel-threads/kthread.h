#ifndef XV6_PUBLIC_KTHREAD_H
#define XV6_PUBLIC_KTHREAD_H

#define NTHREAD			16

//adding function prototypes, implementations found in proc.c
int kthread_create(void*(*start_func)(), void* stack, int stack_size);
int kthread_id();
void kthread_exit();
int kthread_join(int thread_id);
void            kill_others(void);
void            kill_all(void);

#endif
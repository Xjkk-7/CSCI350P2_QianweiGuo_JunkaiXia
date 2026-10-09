#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return proc->pid;
}

// edited by: Junkai Xia
// Use growproc_sbrk to synchronize the old-size read and memory update.
int
sys_sbrk(void)
{
  int n;

  if(argint(0, &n) < 0)
    return -1;

  return growproc_sbrk(n);
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;

  acquire(&tickslock);
  ticks0 = ticks;

  while(ticks - ticks0 < n){
    if(proc->killed || thread->killed){
      release(&tickslock);
      return -1;
    }

    sleep_killable(&ticks, &tickslock);
  }

  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int 
sys_procdump(void)
{
  procdump();
  return 0;
}

int sys_kthread_mutex_alloc(void) {
  return kthread_mutex_alloc();
}

int sys_kthread_mutex_dealloc(void) {
  int mutex_id;
  if(argint(0, &mutex_id) < 0) {
    return -1;
  }

  return kthread_mutex_dealloc(mutex_id);
}

int sys_kthread_mutex_lock(void) {
  int mutex_id;
  if(argint(0, &mutex_id) < 0) {
    return -1;
  }

  return kthread_mutex_lock(mutex_id);
}

int sys_kthread_mutex_unlock(void) {
  int mutex_id;
  if(argint(0, &mutex_id) < 0) {
    return -1;
  }

  return kthread_mutex_unlock(mutex_id);
}

int
sys_kthread_id(void)
{
  return kthread_id();
}

int
sys_kthread_create(void)
{
  int start_addr;
  int stack_addr;
  int stack_size;

  if(argint(0, &start_addr) < 0 || argint(1, &stack_addr) < 0 ||argint(2, &stack_size) < 0)
    return -1;

  return kthread_create( (void* (*)())start_addr,(void*)stack_addr, stack_size );
}

int
sys_kthread_exit(void)
{
  kthread_exit();
  return 0;  // Not reached.
}

int
sys_kthread_join(void)
{
  int tid;

  if(argint(0, &tid) < 0)
    return -1;

  return kthread_join(tid);
}

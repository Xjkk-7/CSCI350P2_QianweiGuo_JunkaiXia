#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

//edited by: Junkai Xia

void clearThread(struct thread * t);

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct {
  struct spinlock lock;
  struct kthread_mutex_t mutex[MAX_MUTEXES];
} mtable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
int nextmid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
static int kill_others(int for_exec);
static void kill_all(void);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&mtable.lock, "mtable");
}

// Allocate and initialize threads, reusing exited slots only when safe.
struct thread*
allocthread(struct proc *p)
{
  struct thread *t;
  char *sp;

  for(t = p->threads; t < &p->threads[NTHREAD]; t++){
    if(t->state == TUNUSED)
      goto found;
  }

  for(t = p->threads; t < &p->threads[NTHREAD]; t++){
    if(t->state == TZOMBIE &&
       t->joined && t->joiners == 0){
      clearThread(t);
      goto found;
    }
  }

  return 0;

found:
  t->tid = nexttid++;
  t->state = TEMBRYO;
  t->parent = p;
  t->killed = 0;
  t->chan = 0;
  t->joined = 0;
  t->joiners = 0;

  if((t->kstack = kalloc()) == 0){
    t->tid = 0;
    t->parent = 0;
    t->tf = 0;
    t->context = 0;
    t->state = TUNUSED;
    return 0;
  }

  sp = t->kstack + KSTACKSIZE;

  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  return t;
}



//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
// Must hold ptable.lock.
static struct proc*
allocproc(void)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  return 0;

found:
  p->state = USED;
  p->pid = nextpid++;
  p->killed = 0;
  p->lifecycle_owner = 0;
  p->exiting = 0;

  memset(p->threads, 0, sizeof(p->threads));

  t = allocthread(p);
  if(t == 0){
    p->state = UNUSED;
    return 0;
  }

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  struct thread *t;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  acquire(&ptable.lock);

  p = allocproc();
  t = p->threads;
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(t->tf, 0, sizeof(*t->tf));
  t->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  t->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  t->tf->es = t->tf->ds;
  t->tf->ss = t->tf->ds;
  t->tf->eflags = FL_IF;
  t->tf->esp = PGSIZE;
  t->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  t->state = TRUNNABLE;

  release(&ptable.lock);
}

// Synchronize process memory updates and check lifecycle restrictions.
// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  int locked_here = !holding(&ptable.lock);

  if(locked_here)
    acquire(&ptable.lock);

  if(proc->killed || thread->killed || proc->lifecycle_owner)
    goto bad;

  sz = proc->sz;

  if(n > 0){
    if((uint)n >= KERNBASE - sz)
      goto bad;

    if((sz = allocuvm(proc->pgdir, sz, sz + (uint)n)) == 0)
      goto bad;
  } else if(n < 0){
    if(0U - (uint)n > sz)
      goto bad;

    sz = deallocuvm(proc->pgdir, sz, sz + (uint)n);
  }

  proc->sz = sz;
  switchuvm(proc);

  if(locked_here)
    release(&ptable.lock);

  return 0;

bad:
  if(locked_here)
    release(&ptable.lock);

  return -1;
}

// Read the old size and update process memory under one lock.
int
growproc_sbrk(int n)
{
  int addr;

  acquire(&ptable.lock);

  addr = proc->sz;
  if(growproc(n) < 0)
    addr = -1;

  release(&ptable.lock);
  return addr;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct thread *nt;

  acquire(&ptable.lock);
  if(proc->killed || thread->killed || proc->lifecycle_owner){
    release(&ptable.lock);
    return -1;
  }

  // Allocate process.
  if((np = allocproc()) == 0){
    release(&ptable.lock);
    return -1;
  }
  nt = np->threads;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(nt->kstack);
    nt->kstack = 0;
    np->state = UNUSED;
    release(&ptable.lock);
    return -1;
  }

  np->sz = proc->sz;
  np->parent = proc;
  *nt->tf = *thread->tf;

  // Clear %eax so that fork returns 0 in the child.
  nt->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  pid = np->pid;

  nt->state = TRUNNABLE;

  release(&ptable.lock);

  return pid;
}


// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  acquire(&ptable.lock);

  if(proc->exiting && proc->lifecycle_owner != thread){
    release(&ptable.lock);
    killSelf();
    panic("exit: killSelf returned");
  }

  proc->exiting = 1;
  proc->lifecycle_owner = thread;

  kill_all();

  release(&ptable.lock);

  // Siblings have stopped using the process resources.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  wakeup1(proc->parent);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  thread->state = TINVALID;
  proc->state = ZOMBIE;
  proc->lifecycle_owner = 0;

  sched();
  panic("zombie exit");
}

// Release a terminated thread's kernel stack and reset its fields.
void
clearThread(struct thread *t)
{
  if(t->kstack &&
     (t->state == TINVALID || t->state == TZOMBIE))
    kfree(t->kstack);

  t->kstack = 0;
  t->tf = 0;
  t->context = 0;
  t->chan = 0;
  t->tid = 0;
  t->state = TUNUSED;
  t->parent = 0;
  t->killed = 0;
  t->joined = 0;
  t->joiners = 0;
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct thread * t;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;

        for(t = p->threads; t < &p->threads[NTHREAD]; t++)
          clearThread(t);

        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed || thread->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep_killable(proc, &ptable.lock);  //DOC: wait-sleep  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct thread *t;

  for(;;){
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != USED)
          continue;

      for(t = p->threads; t < &p->threads[NTHREAD]; t++){
        if(t->state != TRUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.


        proc = p;
        thread = t;
        switchuvm(p);
		
		 //cprintf("scheduler p loop 2 state=%d\n",p->state);
		
        t->state = TRUNNING;
        swtch(&cpu->scheduler, t->context);
		
				 //cprintf("scheduler p loop 3\n");
		
		
        switchkvm();


        // Process is done running for now.
        // It should have changed its p->state before coming back.
        proc = 0;
        if(p->state != USED)
          t = &p->threads[NTHREAD];
        
        thread = 0;
      }

    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(thread->state == TRUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");

  intena = cpu->intena;
  swtch(&thread->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  thread->state = TRUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
	
  if(proc == 0 || thread == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: 4lock1
    release(lk);
  }

  
  // Go to sleep.
  thread->chan = chan;
  thread->state = TSLEEPING;
  sched();

  // Tidy up.
  thread->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

void
sleep_killable(void *chan, struct spinlock *lk)
{
  if(lk != &ptable.lock){
    acquire(&ptable.lock);
    release(lk);
  }

  if(!proc->killed && !thread->killed)
    sleep(chan, &ptable.lock);

  if(lk != &ptable.lock){
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == USED)
    {
      for(t = p->threads; t < &p->threads[NTHREAD]; t++)
        if((t->state == TSLEEPING || t->state == TBLOCKED) && t->chan == chan)
          t->state = TRUNNABLE;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      for(t = p->threads; t < &p->threads[NTHREAD]; t++)
        if(t->state == TSLEEPING || t->state == TBLOCKED)
          t->state = TRUNNABLE;

      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Kill the threads with of given process with pid.
// Thread won't exit until it returns
// to user space (see trap in trap.c).
void
killSelf(void)
{
  acquire(&ptable.lock);

  thread->state = TINVALID;
  wakeup1(thread);
  wakeup1(proc);

  sched();
  panic("killSelf returned");
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]    "used",
  [ZOMBIE]    "zombie"
  };
 
  int i;
  struct proc *p;
  struct thread *t;
  char *state;//, *threadState;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    cprintf("%d %s %s\n", p->pid, state, p->name);
    for(t = p->threads; t < &p->threads[NTHREAD]; t++){
 

      if(t->state == TSLEEPING){
        getcallerpcs((uint*)t->context->ebp+2, pc);
        for(i=0; i<10 && pc[i] != 0; i++)
          cprintf("%p ", pc[i]);
        cprintf("\n");
      }
    }


  }
}

// edited by: Qianwei Guo
// implemented mutex methods
int kthread_mutex_alloc(void) {
  acquire(&mtable.lock);
  for(int i = 0; i < MAX_MUTEXES; i++) {
    struct kthread_mutex_t *m = &mtable.mutex[i];
    if(m->state == MUNUSED) {
      m->mutex_id = nextmid++;
      m->state = MUNLOCKED;
      m->waiting = 0;

      int id = m->mutex_id;
      release(&mtable.lock);
      return id;
    }
  }

  release(&mtable.lock);
  return -1;
}

int kthread_mutex_dealloc(int mutex_id) {
  acquire(&mtable.lock);
  struct kthread_mutex_t *m = 0;
  for(int i = 0; i < MAX_MUTEXES; i++) {
    if(mtable.mutex[i].state != MUNUSED && mtable.mutex[i].mutex_id == mutex_id) {
      m = &mtable.mutex[i];
      break;
    }
  }

  // mutex doesn't exist or locked or someone is waiting on it
  if(m == 0 || m->state != MUNLOCKED || m->waiting != 0) {
    release(&mtable.lock);
    return -1;
  }
  
  m->mutex_id = 0;
  m->state = MUNUSED;
  m->waiting = 0;
  release(&mtable.lock);
  return 0;
}

int kthread_mutex_lock(int mutex_id) {
  acquire(&mtable.lock);
  struct kthread_mutex_t *m = 0;

  for(int i = 0; i < MAX_MUTEXES; i++) {
    if(mtable.mutex[i].state != MUNUSED && mtable.mutex[i].mutex_id == mutex_id) {
      m = &mtable.mutex[i];
      break;
    }
  }
  // didn't find the mutex
  if(m == 0) {
    release(&mtable.lock);
    return -1;
  }

  // spin lock
  while(m->state == MLOCKED) {
    // process / thread have been killed
    if(proc->killed || thread->killed) {
      release(&mtable.lock);
      return -1;
    }

    m->waiting++;
    acquire(&ptable.lock);
    release(&mtable.lock);

    // change sate and put to sleep
    if(!proc->killed && !thread->killed) {
      thread->chan = m;
      thread->state = TBLOCKED;
      sched();
      thread->chan = 0;
    }

    release(&ptable.lock);
    acquire(&mtable.lock);
    m->waiting--;
  }

  if(m->state != MUNLOCKED || proc->killed || thread->killed) {
    release(&mtable.lock);
    return -1;
  }

  // acquired the lock
  m->state = MLOCKED;
  release(&mtable.lock);
  return 0;
}

int kthread_mutex_unlock(int mutex_id) {
  acquire(&mtable.lock);
  struct kthread_mutex_t *m = 0;
  for(int i = 0; i < MAX_MUTEXES; i++) {
    if(mtable.mutex[i].state != MUNUSED && mtable.mutex[i].mutex_id == mutex_id) {
      m = &mtable.mutex[i];
      break;
    }
  }

  if(m == 0 || m->state != MLOCKED) {
    release(&mtable.lock);
    return -1;
  }
  
  m->state = MUNLOCKED;
  wakeup(m);
  release(&mtable.lock);
  return 0;
}

// Return the calling thread's identifier.
int
kthread_id(void)
{
  if(proc && thread)
    return thread->tid;
  return -1;
}

// Create a runnable thread using the caller-provided user stack.
int
kthread_create(void* (*start_func)(), void* stack, int stack_size)
{
  struct thread *t;
  uint base, top, sp;
  uint return_addr = 0xffffffff;
  int tid;

  base = (uint)stack;

  acquire(&ptable.lock);

  if(proc->killed || thread->killed || proc->lifecycle_owner){
    release(&ptable.lock);
    return -1;
  }

  if(start_func == 0 || (uint)start_func >= proc->sz ||
     stack_size < (int)sizeof(uint) || base >= proc->sz ||
     (uint)stack_size > proc->sz - base){
    release(&ptable.lock);
    return -1;
  }

  top = (base + (uint)stack_size) & ~15U;
  if(top < base + sizeof(uint)){
    release(&ptable.lock);
    return -1;
  }
  sp = top - sizeof(uint);

  t = allocthread(proc);
  if(t == 0){
    release(&ptable.lock);
    return -1;
  }

  if(copyout(proc->pgdir, sp, &return_addr,
             sizeof(return_addr)) < 0){
    kfree(t->kstack);
    memset(t, 0, sizeof(*t));
    t->state = TUNUSED;
    release(&ptable.lock);
    return -1;
  }

  *t->tf = *thread->tf;
  t->tf->esp = sp;
  t->tf->ebp = sp;
  t->tf->eip = (uint)start_func;
  t->tf->eax = 0;

  tid = t->tid;
  t->state = TRUNNABLE;

  release(&ptable.lock);
  return tid;
}

// Terminate the calling thread, exiting the process if it is the last live thread.
void
kthread_exit(void)
{
  struct thread *t;
  int found = 0;

  acquire(&ptable.lock);

  for(t = proc->threads; t < &proc->threads[NTHREAD]; t++){
    if(t != thread &&
       t->state != TUNUSED &&
       t->state != TZOMBIE &&
       t->state != TINVALID){
      found = 1;
      break;
    }
  }

  if(!found){
    release(&ptable.lock);
    exit();
    panic("kthread_exit: exit returned");
  }

  thread->state = TZOMBIE;
  wakeup1(thread);
  wakeup1(proc);

  sched();
  panic("kthread_exit returned");
}

int
kthread_join(int thread_id)
{
  struct thread *t;

  if(thread_id <= 0 || thread_id == thread->tid)
    return -1;

  acquire(&ptable.lock);

  for(t = proc->threads; t < &proc->threads[NTHREAD]; t++)
    if(t->tid == thread_id && t->state != TUNUSED)
      break;

  if(t == &proc->threads[NTHREAD] || t->state == TINVALID){
    release(&ptable.lock);
    return -1;
  }

  t->joiners++;

  while(t->state != TZOMBIE){
    if(proc->killed || thread->killed || t->state == TINVALID){
      t->joiners--;
      release(&ptable.lock);
      return -1;
    }

    sleep_killable(t, &ptable.lock);
  }

  if(t->kstack){
    kfree(t->kstack);
    t->kstack = 0;
    t->tf = 0;
    t->context = 0;
  }

  t->joined = 1;
  t->joiners--;

  release(&ptable.lock);
  return 0;
}

// Request sibling termination and wait until all siblings have stopped.
// Caller must hold ptable.lock.
static int
kill_others(int for_exec)
{
  struct thread *t;
  int found;

  for(t = proc->threads; t < &proc->threads[NTHREAD]; t++){
    if(t == thread || t->state == TUNUSED || t->state == TZOMBIE ||t->state == TINVALID)
      continue;

    t->killed = 1;
    if(t->state == TSLEEPING || t->state == TBLOCKED)
      t->state = TRUNNABLE;
  }

  for(;;){
    if(for_exec && (proc->killed || thread->killed ||
        proc->lifecycle_owner != thread))
      return -1;

    found = 0;

    for(t = proc->threads; t < &proc->threads[NTHREAD]; t++){
      if(t != thread &&
         t->state != TUNUSED &&
         t->state != TZOMBIE &&
         t->state != TINVALID){
        found = 1;
        break;
      }
    }

    if(!found)
      return 0;

    sleep(proc, &ptable.lock);
  }
}

// Caller must hold ptable.lock.
static void
kill_all(void)
{
  proc->killed = 1;
  kill_others(0);
}

// Reserve the process for the calling thread's exec operation.
int
execstart(void)
{
  acquire(&ptable.lock);

  if(proc->killed || thread->killed || proc->lifecycle_owner){
    release(&ptable.lock);
    return -1;
  }

  proc->lifecycle_owner = thread;

  release(&ptable.lock);
  return 0;
}

// Release the exec reservation when loading the new program fails.
void
execabort(void)
{
  acquire(&ptable.lock);

  if(proc->lifecycle_owner == thread && !proc->exiting)
    proc->lifecycle_owner = 0;

  release(&ptable.lock);
}

// Stop siblings and install the new program's address space.
int
execcommit(pde_t *pgdir, uint sz, uint entry, uint sp, char *name)
{
  struct thread *t;
  pde_t *oldpgdir;

  acquire(&ptable.lock);

  if(proc->killed || thread->killed ||
     proc->lifecycle_owner != thread || kill_others(1) < 0){
    release(&ptable.lock);
    return -1;
  }

  for(t = proc->threads; t < &proc->threads[NTHREAD]; t++)
    if(t != thread)
      clearThread(t);

  safestrcpy(proc->name, name, sizeof(proc->name));

  oldpgdir = proc->pgdir;
  proc->pgdir = pgdir;
  proc->sz = sz;

  thread->tf->eip = entry;
  thread->tf->esp = sp;
  thread->tf->ebp = 0;

  switchuvm(proc);
  freevm(oldpgdir);

  proc->lifecycle_owner = 0;

  release(&ptable.lock);
  return 0;
}

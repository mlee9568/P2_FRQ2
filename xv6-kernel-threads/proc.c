#include <stdbool.h>
#include <stdio.h>
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include <stddef.h>


extern char getSharedCounter(int index);

void clearThread(struct thread * t);
void init_mutexes(); 
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;


static struct proc *initproc;

bool pinit_called = false;
int nextpid = 1;
int nexttid = 1;
int nextmid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&mtable.lock, "mtable");
  init_mutexes();

}
void init_mutexes() {
    for (int i = 0; i < MAX_MUTEXES; i++) {
        mtable.mutexes[i].mid = -1;
        mtable.mutexes[i].state = MUNUSED;
        mtable.mutexes[i].owner = -1;
    }
}


struct thread*
mythread(void) {
    return thread;
}

struct thread*
allocthread(struct proc * p)
{
  struct thread *t;
  char *sp;
  int found = 0;

  for(t = p->threads; found != 1 && t < &p->threads[NTHREAD]; t++)
  {
    if(t->state == TUNUSED)
    {
      found = 1;
      t--;
    }
    else if(t->state == TZOMBIE)
    {
      clearThread(t);
      t->state = TUNUSED;
      found = 1;
      t--;
    }
  }

  if(!found)
    return 0;

  t->tid = nexttid++;
  t->state = TEMBRYO;
  t->parent = p;
  t->killed = 0;

  // Allocate kernel stack.
  if((t->kstack = kalloc()) == 0){
    t->state = TUNUSED;
    return 0;
  }
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
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

  t = allocthread(p);

  if(t == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  p->threads[0] = *t;

  for(t = p->threads; t < &p->threads[NTHREAD]; t++)
    t->state = TUNUSED;

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

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;

  sz = proc->sz;
  acquire(&ptable.lock);
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  }
  proc->sz = sz;
  switchuvm(proc);

  release(&ptable.lock);
  return 0;
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

  // Close all open files.
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

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  //kill all threads in this process
  struct thread *new_thread;
  for (new_thread = proc -> threads; new_thread < &proc->threads[NTHREAD]; new_thread++)
  {
    if(new_thread->state != TRUNNING && new_thread->state != TUNUSED && new_thread != thread)
    {
      new_thread->state = TZOMBIE;
    }
  }

  // Jump into the scheduler, never to return.
  kill_all();
  thread->state = TINVALID;
  proc->state = ZOMBIE;

  sched();
  panic("zombie exit");
}

void
clearThread(struct thread * t)
{
  if(t->state == TINVALID || t->state == TZOMBIE)
    kfree(t->kstack);

  t->kstack = 0;
  t->tid = 0;
  t->state = TUNUSED;
  t->parent = 0;
  t->killed = 0;
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
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
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
        if(t->state == TSLEEPING && t->chan == chan)
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
        if(t->state == TSLEEPING)
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
killSelf()
{
  acquire(&ptable.lock);
  wakeup1(thread);
  thread->state = TINVALID; // thread must INVALID itself! - else two cpu's can run on the same thread
  sched();
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

int kthread_create(void *(start_func)(), void *stack, int stack_size) {
  
  acquire(&ptable.lock);

  struct thread *new_thread;
  struct proc *curr_proc = proc;

  //verify args
  if(!start_func || !stack || stack_size <= 0){
    cprintf("one or more invalid args\n");
    release(&ptable.lock);
    return -1;
  }

  // allocate free thread slot in this process
  new_thread = allocthread(curr_proc);

  if(!new_thread){
    cprintf("no free slot in proc %d\n", curr_proc->pid);
    release(&ptable.lock);
    return -1;
  }

  //copy current thread's trap frame
  *new_thread->tf = *thread->tf;

  // find stack address and make esp equal to that 
  new_thread->tf->esp = (uint)stack + stack_size;

  //update base pointer to stack pointer
  new_thread->tf->ebp = new_thread->tf->esp;

  //find address of start function and set instruction pointer to start func
  new_thread->tf->eip = (uint)start_func;

  //mark thread as runnable
  new_thread->state = TRUNNABLE;

  release(&ptable.lock);

  return new_thread->tid;
}

int kthread_id() {
  if (thread == 0 || proc == 0){
    return -1; 
  }
  return thread->tid; 
}

void kthread_exit() {

  acquire(&ptable.lock);

  struct thread* new_thread;
  int found = 0;

  for (new_thread = proc->threads; new_thread < &proc->threads[NTHREAD]; new_thread++)
  {
  //If t is not current thread (because calling thread is current)
  //If t is not Unused, not Zombied and not Invalid
   if (new_thread != thread) {
    if (new_thread->state != TUNUSED && new_thread->state != TZOMBIE && new_thread->state != TINVALID) {
      found = 1;
      break;
    }
   }
  }

  if (found) {
    wakeup1(thread);
  } else {
    kill_all();
    release(&ptable.lock);
    exit();
    wakeup(thread);
  }

  thread->state = TZOMBIE;
  sched();
  release(&ptable.lock);
}

int kthread_join(int thread_id) {

  acquire(&ptable.lock);

  if (thread_id < 0 || thread_id == thread->tid || thread_id >= nexttid) {
    //cprintf("thread id check \n");
    release(&ptable.lock);
    return -1;
  }

  int found = 0;
  struct thread *new_thread;

  //Loop through all threads to find target thread id(parameter)
  //Make t point target thread with thread_id
  for (new_thread = proc->threads; new_thread < &proc->threads[NTHREAD]; new_thread++)
  {
    if (new_thread->tid == thread_id)
    {
      if (new_thread->parent != proc) {
        release(&ptable.lock);
        return -1; 
      }
      found = 1;
      break;
      release(&ptable.lock);
    }
  }
  
  if (!found) {
    release(&ptable.lock);
    return -1;
  }

  //While (t->t_id = thread_id and valid)
  while (new_thread->tid == thread_id && new_thread->state != TZOMBIE && new_thread->state != TUNUSED && new_thread->state != TINVALID )
  {
    //Make t sleep using sleep method with a lock
    sleep(new_thread, &ptable.lock);
    // release(&ptable.lock);
  }

  //If state of t is zombie
  if (new_thread->state == TZOMBIE)
  {
    clearThread(new_thread);
    // release(&ptable.lock);
  }

  release(&ptable.lock);
  return 0;
}


void kill_all(void) {

 struct thread *new_thread;
 for (new_thread = proc->threads; new_thread < &proc->threads[NTHREAD]; new_thread++)
 {
  //If ( thread t is not current thread and not running and not unused)->
  if (new_thread != thread && new_thread->state != TRUNNING && new_thread->state!= TUNUSED) 
  {
    new_thread->state = TZOMBIE;
  }
 }
 //Make current thread zombie -> find current thread and change its state
  thread->state = TZOMBIE;
  //Kill process -> proc->killed = 1
  proc->killed = 1;
}

void kill_others(void)
{
  acquire(&ptable.lock);
  struct thread *new_thread;
  
  for (new_thread = proc->threads; new_thread < &proc->threads[NTHREAD]; new_thread++)
  {
    //If ( thread t is not current thread and not running and not unused)
  if(new_thread != thread && new_thread->state != TRUNNING && new_thread->state != TUNUSED)
  {
    new_thread->state = TZOMBIE; //Make it zombie
  }
  }
  release(&ptable.lock);
}

kthread_mutex_t mutexes[MAX_MUTEXES];

int kthread_mutex_alloc() {

    if (pinit_called == false) {
        pinit();
        pinit_called = true;
    }
    acquire(&mtable.lock);

    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (mtable.mutexes[i].state == MUNUSED) {
            mtable.mutexes[i].mid = nextmid++;
            mtable.mutexes[i].state = MUNLOCKED;
            mtable.mutexes[i].owner = -1;
            release(&mtable.lock);
            return mtable.mutexes[i].mid;
        }
    }
    release(&mtable.lock);
    return -1; 
}
int kthread_mutex_dealloc(int mutex_id) {
    struct kthread_mutex *m = NULL;

    acquire(&mtable.lock);  
    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (mtable.mutexes[i].mid == mutex_id) {
            m = &mtable.mutexes[i];
            break;
        }
    }
    if (!m || m->state == MLOCKED) {
        release(&mtable.lock);
        return -1;
    }
    m->mid = 0;
    m->state = MUNUSED;
    m->owner = -1;  

    release(&mtable.lock);  
    return 0;
}



int kthread_mutex_lock(int mutex_id) {
    acquire(&mtable.lock);  

    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (mtable.mutexes[i].mid == mutex_id) {
            if (mtable.mutexes[i].state == MUNUSED) {
                release(&mtable.lock);
                return -1;
            }

            for (; mtable.mutexes[i].state == MLOCKED;) {
              sleep((void*)&mtable.mutexes[i], &mtable.lock);
            }
            mtable.mutexes[i].state = MLOCKED;
            mtable.mutexes[i].owner = thread->tid;

            release(&mtable.lock);
            return 0;
        }
    }

    release(&mtable.lock);  
    return -1;
}


int kthread_mutex_unlock(int mutex_id) {
    acquire(&mtable.lock);  

    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (mtable.mutexes[i].mid == mutex_id) {
            if (mtable.mutexes[i].state != MLOCKED) {
                release(&mtable.lock);
                return -1;
            }

            mtable.mutexes[i].state = MUNLOCKED;
            mtable.mutexes[i].owner = -1;

            wakeup((void*)&mtable.mutexes[i]);

            release(&mtable.lock);
            return 0;
        }
    }
    release(&mtable.lock); 
    return -1;
}



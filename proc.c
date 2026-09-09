#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct runqueue {
  struct proc *head[NPRIO];
  struct proc *tail[NPRIO];
  int nready;
};

static struct runqueue runq[NCPU];
static uint last_aged;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

static int
quantum_for(int prio)
{
  return QUANTUM_BASE * (NPRIO - prio);
}

static void
rq_push(int cpu, struct proc *p)
{
  struct runqueue *rq = &runq[cpu];
  int prio = p->prio;

  p->rqnext = 0;
  p->rqcpu = cpu;
  if(rq->tail[prio])
    rq->tail[prio]->rqnext = p;
  else
    rq->head[prio] = p;
  rq->tail[prio] = p;
  rq->nready++;
}

static struct proc*
rq_pop(int cpu)
{
  struct runqueue *rq = &runq[cpu];
  struct proc *p;
  int prio;

  for(prio = 0; prio < NPRIO; prio++){
    if((p = rq->head[prio]) != 0){
      rq->head[prio] = p->rqnext;
      if(rq->head[prio] == 0)
        rq->tail[prio] = 0;
      p->rqnext = 0;
      rq->nready--;
      return p;
    }
  }
  return 0;
}

static int
least_loaded_cpu(void)
{
  int i, best = 0, bestn;

  bestn = runq[0].nready;
  for(i = 1; i < ncpu; i++){
    if(runq[i].nready < bestn){
      bestn = runq[i].nready;
      best = i;
    }
  }
  return best;
}

static struct proc*
rq_steal(int self)
{
  int i, best = -1, bestn = 1;

  for(i = 0; i < ncpu; i++){
    if(i == self)
      continue;
    if(runq[i].nready > bestn){
      bestn = runq[i].nready;
      best = i;
    }
  }
  if(best < 0)
    return 0;
  return rq_pop(best);
}

static void
rq_age(uint elapsed)
{
  struct proc *pending[NPROC];
  struct proc *p;
  int c, n, i;

  for(c = 0; c < ncpu; c++){
    n = 0;
    while(n < NPROC && (p = rq_pop(c)) != 0)
      pending[n++] = p;
    for(i = 0; i < n; i++){
      p = pending[i];
      p->waited += elapsed;
      if(p->waited >= AGING_TICKS && p->prio > 0){
        p->prio--;
        p->waited = 0;
      }
      rq_push(c, p);
    }
  }
}

void
scheduler_tick(uint now)
{
  uint elapsed;

  if(now - last_aged < AGING_INTERVAL)
    return;

  acquire(&ptable.lock);
  elapsed = now - last_aged;
  last_aged = now;
  rq_age(elapsed);
  release(&ptable.lock);
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->pgdir = 0;
  p->isthread = 0;
  p->ustack = 0;
  p->base_prio = NPRIO / 2;
  p->prio = p->base_prio;
  p->slice = quantum_for(p->prio);
  p->waited = 0;
  p->rqcpu = 0;
  p->rqnext = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  rq_push(least_loaded_cpu(), p);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  syncsize();
  switchuvm(curproc);
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
  struct proc *curproc = myproc();

  if(addrspace_shared())
    return -1;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = cowuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  np->base_prio = curproc->base_prio;
  np->prio = np->base_prio;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  rq_push(least_loaded_cpu(), np);

  release(&ptable.lock);

  return pid;
}

static int
pgdir_users(pde_t *pgdir)
{
  struct proc *p;
  int n = 0;

  if(pgdir == 0)
    return 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state != UNUSED && p->pgdir == pgdir)
      n++;
  return n;
}

int
addrspace_shared(void)
{
  int n;

  acquire(&ptable.lock);
  n = pgdir_users(myproc()->pgdir);
  release(&ptable.lock);
  return n > 1;
}

void
syncsize(void)
{
  struct proc *q;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(q = ptable.proc; q < &ptable.proc[NPROC]; q++)
    if(q != curproc && q->state != UNUSED && q->pgdir == curproc->pgdir)
      q->sz = curproc->sz;
  release(&ptable.lock);
}

int
clone(void (*fn)(void*), void *arg, void *stack)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  uint sp;

  if((uint)stack % PGSIZE != 0)
    return -1;
  if((uint)stack >= curproc->sz || (uint)stack + PGSIZE > curproc->sz)
    return -1;

  if((np = allocproc()) == 0)
    return -1;

  np->pgdir = curproc->pgdir;
  np->sz = curproc->sz;
  np->parent = curproc;
  np->isthread = 1;
  np->ustack = stack;
  np->base_prio = curproc->base_prio;
  np->prio = np->base_prio;
  *np->tf = *curproc->tf;

  sp = (uint)stack + PGSIZE;
  sp -= 4;
  *(uint*)sp = (uint)arg;
  sp -= 4;
  *(uint*)sp = 0xffffffff;

  np->tf->eip = (uint)fn;
  np->tf->esp = sp;
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);
  np->state = RUNNABLE;
  rq_push(least_loaded_cpu(), np);
  release(&ptable.lock);

  return pid;
}

int
join(void **stack)
{
  struct proc *p;
  struct proc *curproc = myproc();
  int havethreads, pid;
  void *found;

  acquire(&ptable.lock);
  for(;;){
    havethreads = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc || !p->isthread)
        continue;
      havethreads = 1;
      if(p->state == ZOMBIE){
        pid = p->pid;
        found = p->ustack;
        kfree(p->kstack);
        p->kstack = 0;
        p->pgdir = 0;
        p->isthread = 0;
        p->ustack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        if(stack)
          *stack = found;
        return pid;
      }
    }

    if(!havethreads || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);
  }
}

int
futex_wait(void *addr, int expected)
{
  struct proc *curproc = myproc();
  uint pa;

  if((uint)addr % 4 != 0 || (uint)addr + 4 > curproc->sz)
    return -1;

  if(*(volatile int*)addr != expected)
    return 0;

  pa = uva2pa(curproc->pgdir, (uint)addr);
  if(pa == 0)
    return -1;

  acquire(&ptable.lock);
  if(*(volatile int*)addr != expected){
    release(&ptable.lock);
    return 0;
  }
  curproc->chan = (void*)pa;
  curproc->state = SLEEPING;
  sched();
  curproc->chan = 0;
  release(&ptable.lock);
  return 0;
}

int
futex_wake(void *addr, int n)
{
  struct proc *p;
  struct proc *curproc = myproc();
  uint pa;
  int woken = 0;

  if((uint)addr % 4 != 0 || (uint)addr + 4 > curproc->sz)
    return -1;

  pa = uva2pa(curproc->pgdir, (uint)addr);
  if(pa == 0)
    return 0;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == (void*)pa){
      p->state = RUNNABLE;
      rq_push(p->rqcpu, p);
      woken++;
      if(n > 0 && woken >= n)
        break;
    }
  }
  release(&ptable.lock);
  return woken;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        if(p->pgdir && pgdir_users(p->pgdir) == 1)
          freevm(p->pgdir);
        p->pgdir = 0;
        p->isthread = 0;
        p->ustack = 0;
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
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
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
  struct cpu *c = mycpu();
  int id;

  c->proc = 0;
  id = cpuid();

  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    p = rq_pop(id);
    if(p == 0)
      p = rq_steal(id);

    if(p != 0){
      if(p->state != RUNNABLE)
        panic("scheduler: queued process is not runnable");

      p->rqcpu = id;
      p->prio = p->base_prio;
      p->waited = 0;
      p->slice = quantum_for(p->prio);

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

int
setpriority(int prio)
{
  struct proc *p = myproc();

  if(prio < 0 || prio >= NPRIO)
    return -1;

  acquire(&ptable.lock);
  p->base_prio = prio;
  p->prio = prio;
  p->slice = quantum_for(prio);
  release(&ptable.lock);
  return 0;
}

int
getpriority(void)
{
  return myproc()->base_prio;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  rq_push(cpuid(), myproc());
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
  struct proc *p = myproc();
  
  if(p == 0)
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
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      rq_push(p->rqcpu, p);
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

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        rq_push(p->rqcpu, p);
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
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
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

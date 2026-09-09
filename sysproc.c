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
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;
  struct proc *curproc = myproc();

  if(argint(0, &n) < 0)
    return -1;
  addr = curproc->sz;
  if(n < 0){
    if(growproc(n) < 0)
      return -1;
    return addr;
  }
  if(curproc->sz + n >= KERNBASE || curproc->sz + n < curproc->sz)
    return -1;
  curproc->sz += n;
  syncsize();
  return addr;
}

int
sys_freemem(void)
{
  return freemem();
}

int
sys_setpriority(void)
{
  int prio;

  if(argint(0, &prio) < 0)
    return -1;
  return setpriority(prio);
}

int
sys_getpriority(void)
{
  return getpriority();
}

int
sys_clone(void)
{
  int fn, arg, stack;

  if(argint(0, &fn) < 0 || argint(1, &arg) < 0 || argint(2, &stack) < 0)
    return -1;
  return clone((void(*)(void*))fn, (void*)arg, (void*)stack);
}

int
sys_join(void)
{
  char *p;
  void *stack;
  int r;

  if(argptr(0, &p, sizeof(void*)) < 0)
    return -1;
  r = join(&stack);
  if(r < 0)
    return -1;
  *(void**)p = stack;
  return r;
}

int
sys_futex_wait(void)
{
  char *addr;
  int expected;

  if(argptr(0, &addr, 4) < 0 || argint(1, &expected) < 0)
    return -1;
  return futex_wait(addr, expected);
}

int
sys_futex_wake(void)
{
  char *addr;
  int n;

  if(argptr(0, &addr, 4) < 0 || argint(1, &n) < 0)
    return -1;
  return futex_wake(addr, n);
}

int
sys_getncpu(void)
{
  return ncpu;
}

int
sys_getcpu(void)
{
  int id;

  pushcli();
  id = cpuid();
  popcli();
  return id;
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
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
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

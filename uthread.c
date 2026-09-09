#include "types.h"
#include "stat.h"
#include "user.h"
#include "uthread.h"

#define MAXTHREADS 32

static struct {
  void *stack;
  int used;
} slots[MAXTHREADS];

static struct mutex alloc_lock;
static int initialized;

void
thread_init(void)
{
  int i;

  for(i = 0; i < MAXTHREADS; i++){
    slots[i].stack = 0;
    slots[i].used = 0;
  }
  mutex_init(&alloc_lock);
  initialized = 1;
}

static void*
aligned_stack(void)
{
  char *raw;
  uint addr;

  raw = malloc(UTHREAD_STACK * 2);
  if(raw == 0)
    return 0;
  addr = ((uint)raw + UTHREAD_STACK - 1) & ~(UTHREAD_STACK - 1);
  return (void*)addr;
}

int
thread_create(void (*fn)(void*), void *arg)
{
  void *stack;
  int tid, i;

  if(!initialized)
    thread_init();

  stack = aligned_stack();
  if(stack == 0)
    return -1;

  mutex_lock(&alloc_lock);
  for(i = 0; i < MAXTHREADS; i++){
    if(!slots[i].used){
      slots[i].used = 1;
      slots[i].stack = stack;
      break;
    }
  }
  mutex_unlock(&alloc_lock);

  if(i == MAXTHREADS)
    return -1;

  tid = clone(fn, arg, stack);
  if(tid < 0){
    mutex_lock(&alloc_lock);
    slots[i].used = 0;
    mutex_unlock(&alloc_lock);
    return -1;
  }
  return tid;
}

int
thread_join(void)
{
  void *stack;
  int tid, i;

  tid = join(&stack);
  if(tid < 0)
    return -1;

  mutex_lock(&alloc_lock);
  for(i = 0; i < MAXTHREADS; i++){
    if(slots[i].used && slots[i].stack == stack)
      slots[i].used = 0;
  }
  mutex_unlock(&alloc_lock);
  return tid;
}

void
mutex_init(struct mutex *m)
{
  m->locked = 0;
}

void
mutex_lock(struct mutex *m)
{
  while(uxchg(&m->locked, 1) != 0)
    futex_wait((void*)&m->locked, 1);
}

int
mutex_trylock(struct mutex *m)
{
  return uxchg(&m->locked, 1) == 0;
}

void
mutex_unlock(struct mutex *m)
{
  uxchg(&m->locked, 0);
  futex_wake((void*)&m->locked, 1);
}

void
cond_init(struct cond *c)
{
  c->seq = 0;
}

void
cond_wait(struct cond *c, struct mutex *m)
{
  uint seq;

  seq = c->seq;
  mutex_unlock(m);
  futex_wait((void*)&c->seq, seq);
  mutex_lock(m);
}

void
cond_signal(struct cond *c)
{
  uxchg(&c->seq, c->seq + 1);
  futex_wake((void*)&c->seq, 1);
}

void
cond_broadcast(struct cond *c)
{
  uxchg(&c->seq, c->seq + 1);
  futex_wake((void*)&c->seq, 0);
}

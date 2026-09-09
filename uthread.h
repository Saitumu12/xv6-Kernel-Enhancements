#ifndef UTHREAD_H
#define UTHREAD_H

#define UTHREAD_STACK 4096

struct mutex {
  volatile uint locked;
};

struct cond {
  volatile uint seq;
};

static inline uint
uxchg(volatile uint *addr, uint newval)
{
  uint result;

  asm volatile("lock; xchgl %0, %1"
               : "+m" (*addr), "=a" (result)
               : "1" (newval)
               : "cc");
  return result;
}

void thread_init(void);
int thread_create(void (*fn)(void*), void *arg);
int thread_join(void);

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int mutex_trylock(struct mutex *m);

void cond_init(struct cond *c);
void cond_wait(struct cond *c, struct mutex *m);
void cond_signal(struct cond *c);
void cond_broadcast(struct cond *c);

#endif

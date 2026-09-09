#include "types.h"
#include "stat.h"
#include "user.h"
#include "uthread.h"

#define NTHREAD 4
#define NINCR 20000
#define NITEMS 200
#define BUFSZ 8

static int failures;

static void
check(int ok, const char *what)
{
  if(ok){
    printf(1, "  ok    %s\n", what);
  } else {
    printf(1, "  FAIL  %s\n", what);
    failures++;
  }
}

static volatile int shared_counter;
static struct mutex counter_lock;
static volatile int threads_started;

static void
increment_worker(void *arg)
{
  int i;

  for(i = 0; i < NINCR; i++){
    mutex_lock(&counter_lock);
    shared_counter++;
    mutex_unlock(&counter_lock);
  }
  exit();
}

static void
mutex_protects_a_counter(void)
{
  int i, created;

  shared_counter = 0;
  mutex_init(&counter_lock);

  created = 0;
  for(i = 0; i < NTHREAD; i++){
    if(thread_create(increment_worker, 0) >= 0)
      created++;
  }
  for(i = 0; i < created; i++)
    thread_join();

  check(created == NTHREAD, "all threads were created");
  printf(1, "  counter reached %d, expected %d\n", shared_counter, created * NINCR);
  check(shared_counter == created * NINCR,
        "a mutex makes concurrent increments exact");
}

static volatile int visible_flag;

static void
flag_worker(void *arg)
{
  visible_flag = (int)(uint)arg;
  exit();
}

static void
threads_share_the_address_space(void)
{
  visible_flag = 0;
  if(thread_create(flag_worker, (void*)1234) < 0){
    check(0, "create flag thread");
    return;
  }
  thread_join();
  check(visible_flag == 1234, "a thread's write to a global is visible to its creator");
}

static struct mutex qlock;
static struct cond notempty;
static struct cond notfull;
static int queue[BUFSZ];
static int qcount;
static int qhead;
static int qtail;
static volatile int consumed_sum;
static volatile int produced_sum;

static void
producer(void *arg)
{
  int i;

  for(i = 1; i <= NITEMS; i++){
    mutex_lock(&qlock);
    while(qcount == BUFSZ)
      cond_wait(&notfull, &qlock);
    queue[qtail] = i;
    qtail = (qtail + 1) % BUFSZ;
    qcount++;
    produced_sum += i;
    cond_signal(&notempty);
    mutex_unlock(&qlock);
  }
  exit();
}

static void
consumer(void *arg)
{
  int i, v;

  for(i = 0; i < NITEMS; i++){
    mutex_lock(&qlock);
    while(qcount == 0)
      cond_wait(&notempty, &qlock);
    v = queue[qhead];
    qhead = (qhead + 1) % BUFSZ;
    qcount--;
    consumed_sum += v;
    cond_signal(&notfull);
    mutex_unlock(&qlock);
  }
  exit();
}

static void
condition_variables_move_every_item(void)
{
  int expected;

  mutex_init(&qlock);
  cond_init(&notempty);
  cond_init(&notfull);
  qcount = 0;
  qhead = 0;
  qtail = 0;
  consumed_sum = 0;
  produced_sum = 0;

  if(thread_create(consumer, 0) < 0 || thread_create(producer, 0) < 0){
    check(0, "create producer and consumer");
    return;
  }
  thread_join();
  thread_join();

  expected = NITEMS * (NITEMS + 1) / 2;
  printf(1, "  produced %d consumed %d expected %d\n",
         produced_sum, consumed_sum, expected);
  check(produced_sum == expected, "the producer produced every item");
  check(consumed_sum == expected, "the consumer received every item exactly once");
  check(qcount == 0, "the queue drained");
}

static struct mutex block_lock;
static struct cond gate;
static volatile int waiters_awake;
static volatile int gate_open;

static void
gate_waiter(void *arg)
{
  mutex_lock(&block_lock);
  while(!gate_open)
    cond_wait(&gate, &block_lock);
  waiters_awake++;
  mutex_unlock(&block_lock);
  exit();
}

static void
broadcast_wakes_every_waiter(void)
{
  int i, created;

  mutex_init(&block_lock);
  cond_init(&gate);
  waiters_awake = 0;
  gate_open = 0;

  created = 0;
  for(i = 0; i < NTHREAD; i++){
    if(thread_create(gate_waiter, 0) >= 0)
      created++;
  }

  sleep(20);

  mutex_lock(&block_lock);
  gate_open = 1;
  cond_broadcast(&gate);
  mutex_unlock(&block_lock);

  for(i = 0; i < created; i++)
    thread_join();

  check(waiters_awake == created, "broadcast released every waiter");
}

static volatile int cpu_seen[8];

static void
cpu_recorder(void *arg)
{
  int i;
  volatile int sink = 0;

  for(i = 0; i < 400000; i++)
    sink += i;
  cpu_seen[getcpu() & 7] = 1;
  exit();
}

static void
threads_run_on_several_cpus(void)
{
  int i, created, distinct;

  for(i = 0; i < 8; i++)
    cpu_seen[i] = 0;

  created = 0;
  for(i = 0; i < NTHREAD; i++){
    if(thread_create(cpu_recorder, 0) >= 0)
      created++;
  }
  for(i = 0; i < created; i++)
    thread_join();

  distinct = 0;
  for(i = 0; i < 8; i++)
    distinct += cpu_seen[i];

  printf(1, "  threads observed on %d cpus\n", distinct);
  check(distinct >= 2, "threads of one process run on more than one cpu");
}

#define PGSIZE 4096
#define STRESS_PAGES 32

static char *cow_region;
static char *lazy_region;

static void
stress_writer(void *arg)
{
  int i;

  for(i = 0; i < STRESS_PAGES; i++)
    cow_region[i * PGSIZE] = 'w';
  for(i = 0; i < STRESS_PAGES; i++)
    lazy_region[i * PGSIZE] = 'l';
  exit();
}

static void
threads_faulting_on_the_same_pages(void)
{
  int fds[2];
  int i, created, pid;
  char c;

  cow_region = sbrk(STRESS_PAGES * PGSIZE);
  for(i = 0; i < STRESS_PAGES; i++)
    cow_region[i * PGSIZE] = 'p';
  lazy_region = sbrk(STRESS_PAGES * PGSIZE);

  pipe(fds);
  pid = fork();
  if(pid == 0){
    close(fds[0]);
    created = 0;
    for(i = 0; i < NTHREAD; i++){
      if(thread_create(stress_writer, 0) >= 0)
        created++;
    }
    for(i = 0; i < created; i++)
      thread_join();

    for(i = 0; i < STRESS_PAGES; i++){
      if(cow_region[i * PGSIZE] != 'w' || lazy_region[i * PGSIZE] != 'l'){
        write(fds[1], "n", 1);
        exit();
      }
    }
    write(fds[1], created == NTHREAD ? "y" : "n", 1);
    exit();
  }

  close(fds[1]);
  if(read(fds[0], &c, 1) != 1)
    c = 'n';
  close(fds[0]);
  wait();

  check(c == 'y', "threads racing on the same cow and lazy pages all survive");
  for(i = 0; i < STRESS_PAGES; i++){
    if(cow_region[i * PGSIZE] != 'p')
      break;
  }
  check(i == STRESS_PAGES, "the parent is untouched by the child's racing threads");
  sbrk(-(2 * STRESS_PAGES * PGSIZE));
}

static void
joining_with_no_threads_fails(void)
{
  void *stack;

  check(join(&stack) < 0, "join with no outstanding thread returns an error");
}

int
main(void)
{
  printf(1, "threadtest: starting\n");
  thread_init();

  threads_share_the_address_space();
  mutex_protects_a_counter();
  condition_variables_move_every_item();
  broadcast_wakes_every_waiter();
  threads_run_on_several_cpus();
  threads_faulting_on_the_same_pages();
  joining_with_no_threads_fails();

  if(failures == 0)
    printf(1, "threadtest: OK\n");
  else
    printf(1, "threadtest: FAIL (%d)\n", failures);
  exit();
}

#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

#define WORK_CHUNK 20000
#define WARMUP 20
#define DURATION 120
#define NPRIO 4
#define MAXCHILD 16

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

struct result {
  int work;
  int cpu;
  int cpumask;
  int prio;
};

static int seen_cpus;

static int
spin_until(int deadline)
{
  volatile int sink = 0;
  int chunks = 0;
  int i;

  seen_cpus = 0;
  while(uptime() < deadline){
    for(i = 0; i < WORK_CHUNK; i++)
      sink += i;
    seen_cpus |= 1 << (getcpu() & 7);
    chunks++;
  }
  return chunks;
}

static void
run_children(int n, const int *prios, struct result *out)
{
  int fds[2];
  int start, deadline;
  int i;
  struct result r;

  pipe(fds);
  start = uptime() + WARMUP;
  deadline = start + DURATION;

  for(i = 0; i < n; i++){
    if(fork() == 0){
      close(fds[0]);
      setpriority(prios[i]);
      while(uptime() < start)
        ;
      r.work = spin_until(deadline);
      r.cpu = getcpu();
      r.cpumask = seen_cpus;
      r.prio = prios[i];
      write(fds[1], (char*)&r, sizeof(r));
      close(fds[1]);
      exit();
    }
  }

  close(fds[1]);
  for(i = 0; i < n; i++){
    if(read(fds[0], (char*)&out[i], sizeof(out[i])) != sizeof(out[i]))
      out[i].work = -1;
  }
  close(fds[0]);
  for(i = 0; i < n; i++)
    wait();
}

static void
higher_priority_gets_more_cpu(void)
{
  int prios[MAXCHILD];
  struct result r[MAXCHILD];
  int high, low, i, each, total;

  for(i = 0; i < MAXCHILD; i++)
    prios[i] = 3;

  each = 2 * getncpu();
  if(each < 1)
    each = 1;
  if(each > MAXCHILD / 2)
    each = MAXCHILD / 2;
  total = each * 2;

  for(i = 0; i < each; i++)
    prios[i] = 0;

  run_children(total, prios, r);

  high = 0;
  low = 0;
  for(i = 0; i < total; i++){
    if(r[i].work < 0){
      check(0, "child reported work");
      return;
    }
    if(r[i].prio == 0)
      high += r[i].work;
    else
      low += r[i].work;
  }
  printf(1, "  %d cpus, %d spinners per level: priority 0 did %d units, priority 3 did %d units\n",
         getncpu(), each, high, low);

  check(high > low + low / 4, "a higher priority process gets more cpu than a lower one");
  check(low > 0, "a lower priority process is not starved");
}

static void
equal_priority_is_round_robin(void)
{
  int prios[MAXCHILD];
  struct result r[MAXCHILD];
  int i, min, max, n;

  for(i = 0; i < MAXCHILD; i++)
    prios[i] = 1;
  n = 2 * getncpu();
  if(n < 2)
    n = 2;
  if(n > MAXCHILD)
    n = MAXCHILD;

  run_children(n, prios, r);

  min = r[0].work;
  max = r[0].work;
  for(i = 1; i < n; i++){
    if(r[i].work < min)
      min = r[i].work;
    if(r[i].work > max)
      max = r[i].work;
  }
  printf(1, "  equal priority spread: min %d max %d\n", min, max);

  check(min > 0, "every equal priority process runs");
  check(max <= min * 2, "equal priority processes get comparable cpu");
}

static void
work_lands_on_more_than_one_cpu(void)
{
  int prios[MAXCHILD];
  struct result r[MAXCHILD];
  int i, mask, distinct, n;

  for(i = 0; i < MAXCHILD; i++)
    prios[i] = 1;
  n = 2 * getncpu();
  if(n < 2)
    n = 2;
  if(n > MAXCHILD)
    n = MAXCHILD;

  run_children(n, prios, r);

  mask = 0;
  for(i = 0; i < n; i++)
    mask |= r[i].cpumask;

  distinct = 0;
  for(i = 0; i < 8; i++){
    if(mask & (1 << i))
      distinct++;
  }
  printf(1, "  work ran on %d distinct cpus (mask 0x%x)\n", distinct, mask);
  check(distinct >= 2, "runnable work is spread over more than one cpu");
}

static void
a_spinner_does_not_block_the_shell(void)
{
  int pid;
  int before, after;

  pid = fork();
  if(pid == 0){
    setpriority(0);
    spin_until(uptime() + DURATION);
    exit();
  }

  before = uptime();
  sleep(10);
  after = uptime();

  kill(pid);
  wait();

  check(after - before >= 10, "a sleeping process still wakes while a spinner runs");
}

static void
priority_bounds_are_enforced(void)
{
  int old;

  old = getpriority();
  check(setpriority(-1) < 0, "a negative priority is rejected");
  check(setpriority(NPRIO) < 0, "an out of range priority is rejected");
  check(getpriority() == old, "a rejected setpriority leaves the priority alone");
  check(setpriority(2) == 0, "a valid priority is accepted");
  check(getpriority() == 2, "getpriority reports what was set");
  setpriority(old);
}

static void
priority_is_inherited_by_children(void)
{
  int fds[2];
  int got;

  setpriority(2);
  pipe(fds);
  if(fork() == 0){
    int p = getpriority();
    write(fds[1], (char*)&p, sizeof(p));
    close(fds[1]);
    exit();
  }
  close(fds[1]);
  read(fds[0], (char*)&got, sizeof(got));
  close(fds[0]);
  wait();
  check(got == 2, "a child inherits its parent's priority");
  setpriority(1);
}

int
main(void)
{
  printf(1, "schedtest: starting\n");

  priority_bounds_are_enforced();
  priority_is_inherited_by_children();
  higher_priority_gets_more_cpu();
  equal_priority_is_round_robin();
  work_lands_on_more_than_one_cpu();
  a_spinner_does_not_block_the_shell();

  if(failures == 0)
    printf(1, "schedtest: OK\n");
  else
    printf(1, "schedtest: FAIL (%d)\n", failures);
  exit();
}

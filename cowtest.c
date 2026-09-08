#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define PGSIZE 4096
#define PAGES 256
#define SLACK 32

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

static char
pattern(int i)
{
  return (char)('a' + (i % 26));
}

static char*
alloc_and_fill(int pages)
{
  char *p;
  int i;

  p = sbrk(pages * PGSIZE);
  if(p == (char*)-1)
    return 0;
  for(i = 0; i < pages; i++)
    p[i * PGSIZE] = pattern(i);
  return p;
}

static int
intact(char *p, int pages)
{
  int i;

  for(i = 0; i < pages; i++){
    if(p[i * PGSIZE] != pattern(i))
      return 0;
  }
  return 1;
}

static void
measure_fork(int pages, int *at_fork, int *after_reads, int *after_writes, int *after_exit)
{
  char *p;
  int mid;
  int go[2], done[2];
  int pid, i;
  char c;

  p = alloc_and_fill(pages);
  mid = freemem();

  pipe(go);
  pipe(done);

  pid = fork();
  if(pid == 0){
    close(go[1]);
    close(done[0]);

    read(go[0], &c, 1);
    write(done[1], intact(p, pages) ? "y" : "n", 1);

    read(go[0], &c, 1);
    for(i = 0; i < pages; i++)
      p[i * PGSIZE] = 'Z';
    write(done[1], "w", 1);

    read(go[0], &c, 1);
    exit();
  }

  close(go[0]);
  close(done[1]);

  *at_fork = mid - freemem();

  write(go[1], "g", 1);
  read(done[0], &c, 1);
  if(c != 'y')
    check(0, "child sees the parent's data after fork");
  *after_reads = mid - freemem();

  write(go[1], "g", 1);
  read(done[0], &c, 1);
  *after_writes = mid - freemem();
  check(intact(p, pages), "the parent's pages are untouched by the child");

  write(go[1], "g", 1);
  close(go[1]);
  close(done[0]);
  wait();

  *after_exit = mid - freemem();
  sbrk(-(pages * PGSIZE));
}

static void
fork_cost_is_independent_of_address_space_size(void)
{
  int small_fork, small_reads, small_writes, small_exit;
  int big_fork, big_reads, big_writes, big_exit;

  measure_fork(16, &small_fork, &small_reads, &small_writes, &small_exit);
  measure_fork(PAGES, &big_fork, &big_reads, &big_writes, &big_exit);

  printf(1, "  fork of 16 pages costs %d, fork of %d pages costs %d\n",
         small_fork, PAGES, big_fork);

  check(big_fork - small_fork < PAGES / 8,
        "fork cost does not grow with the size of the address space");
  check(big_reads - small_reads < PAGES / 8,
        "reading in the child copies nothing");
  check(big_writes >= PAGES,
        "writing every page in the child copies every page");
  check(big_exit <= SLACK,
        "the child's copies are freed on exit");
}

static void
parent_writes_are_private_too(void)
{
  char *p;
  int go[2], done[2];
  int pid, i, ok;
  char c;

  p = alloc_and_fill(PAGES);
  pipe(go);
  pipe(done);

  pid = fork();
  if(pid == 0){
    close(go[1]);
    close(done[0]);
    read(go[0], &c, 1);
    ok = intact(p, PAGES);
    write(done[1], ok ? "y" : "n", 1);
    exit();
  }

  close(go[0]);
  close(done[1]);

  for(i = 0; i < PAGES; i++)
    p[i * PGSIZE] = 'P';

  write(go[1], "g", 1);
  read(done[0], &c, 1);
  wait();
  close(go[1]);
  close(done[0]);

  check(c == 'y', "a parent write does not disturb the child");
  sbrk(-(PAGES * PGSIZE));
}

static void
kernel_write_to_a_shared_page_copies(void)
{
  char *p;
  int go[2], done[2];
  int pid, fd, n;
  char c;

  p = alloc_and_fill(PAGES);
  pipe(go);
  pipe(done);

  pid = fork();
  if(pid == 0){
    close(go[1]);
    close(done[0]);
    read(go[0], &c, 1);
    fd = open("README", 0);
    n = read(fd, p, 1024);
    close(fd);
    write(done[1], n > 0 ? "y" : "n", 1);
    exit();
  }

  close(go[0]);
  close(done[1]);
  write(go[1], "g", 1);
  read(done[0], &c, 1);
  wait();
  close(go[1]);
  close(done[0]);

  check(c == 'y', "the child's kernel read into a shared page succeeds");
  check(intact(p, PAGES), "a kernel write to a shared page does not reach the parent");
  sbrk(-(PAGES * PGSIZE));
}

static void
three_levels_of_sharing(void)
{
  char *p;
  int pid, status_ok;
  int fds[2];
  char c;

  p = alloc_and_fill(64);
  pipe(fds);

  pid = fork();
  if(pid == 0){
    int pid2 = fork();
    if(pid2 == 0){
      int i;
      for(i = 0; i < 64; i++)
        p[i * PGSIZE] = 'g';
      exit();
    }
    wait();
    write(fds[1], intact(p, 64) ? "y" : "n", 1);
    exit();
  }

  wait();
  read(fds[0], &c, 1);
  close(fds[0]);
  close(fds[1]);

  status_ok = (c == 'y');
  check(status_ok, "a grandchild write leaves the child's pages alone");
  check(intact(p, 64), "a grandchild write leaves the grandparent's pages alone");
  sbrk(-(64 * PGSIZE));
}

static void
repeated_forks_do_not_leak(void)
{
  int before, after, i, pid;
  char *p;

  p = alloc_and_fill(32);
  before = freemem();

  for(i = 0; i < 20; i++){
    pid = fork();
    if(pid == 0){
      p[0] = 'x';
      p[16 * PGSIZE] = 'y';
      exit();
    }
    wait();
  }

  after = freemem();
  check(after >= before - SLACK, "twenty fork and exit cycles leak nothing");
  check(intact(p, 32), "the parent's data survives twenty children");
  sbrk(-(32 * PGSIZE));
}

int
main(void)
{
  int before, after;

  printf(1, "cowtest: starting\n");
  before = freemem();

  fork_cost_is_independent_of_address_space_size();
  parent_writes_are_private_too();
  kernel_write_to_a_shared_page_copies();
  three_levels_of_sharing();
  repeated_forks_do_not_leak();

  after = freemem();
  check(after >= before - SLACK, "no pages leaked overall");

  if(failures == 0)
    printf(1, "cowtest: OK\n");
  else
    printf(1, "cowtest: FAIL (%d)\n", failures);
  exit();
}

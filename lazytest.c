#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define PGSIZE 4096
#define PAGES 64

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

static void
sbrk_does_not_allocate_eagerly(void)
{
  int before, after;
  char *p;

  before = freemem();
  p = sbrk(PAGES * PGSIZE);
  after = freemem();

  check(p != (char*)-1, "sbrk returns an address");
  check(before - after < PAGES / 2, "growing does not consume the pages up front");

  sbrk(-(PAGES * PGSIZE));
}

static void
touching_allocates_and_zeroes(void)
{
  int before, after, restored, i, zeroed;
  char *p;

  before = freemem();
  p = sbrk(PAGES * PGSIZE);

  zeroed = 1;
  for(i = 0; i < PAGES; i++){
    if(p[i * PGSIZE] != 0)
      zeroed = 0;
  }
  check(zeroed, "faulted pages arrive zeroed");

  for(i = 0; i < PAGES; i++)
    p[i * PGSIZE] = (char)(i + 1);

  after = freemem();
  check(before - after >= PAGES, "touching every page consumes them");

  for(i = 0; i < PAGES; i++){
    if(p[i * PGSIZE] != (char)(i + 1))
      break;
  }
  check(i == PAGES, "contents survive across pages");

  sbrk(-(PAGES * PGSIZE));
  restored = freemem();
  check(restored >= before - 4, "shrinking returns the pages");
}

static void
kernel_writes_into_an_untouched_page(void)
{
  char *buf;
  int fd, n;

  buf = sbrk(PAGES * PGSIZE);
  fd = open("README", 0);
  if(fd < 0){
    check(0, "open README");
    return;
  }

  n = read(fd, buf + 8 * PGSIZE, 512);
  close(fd);

  check(n > 0, "read into an untouched lazy page succeeds");
  check(buf[8 * PGSIZE] != 0, "the kernel actually wrote through the fault");

  sbrk(-(PAGES * PGSIZE));
}

static void
fork_carries_untouched_regions(void)
{
  char *p;
  int pid, i, ok;

  p = sbrk(PAGES * PGSIZE);
  for(i = 0; i < PAGES; i += 2)
    p[i * PGSIZE] = 'p';

  pid = fork();
  if(pid == 0){
    ok = 1;
    for(i = 0; i < PAGES; i += 2){
      if(p[i * PGSIZE] != 'p')
        ok = 0;
    }
    for(i = 1; i < PAGES; i += 2){
      if(p[i * PGSIZE] != 0)
        ok = 0;
      p[i * PGSIZE] = 'c';
    }
    exit();
  }
  wait();
  ok = 1;
  for(i = 1; i < PAGES; i += 2){
    if(p[i * PGSIZE] != 0)
      ok = 0;
  }
  check(ok, "child writes to lazy pages stay private to the child");
  sbrk(-(PAGES * PGSIZE));
}

static void
access_past_the_break_kills_the_process(void)
{
  int fds[2];
  int pid;
  char c;
  char *p;
  int got;

  pipe(fds);
  pid = fork();
  if(pid == 0){
    close(fds[0]);
    write(fds[1], "A", 1);
    p = sbrk(0);
    *(p + 16 * PGSIZE) = 'x';
    write(fds[1], "B", 1);
    close(fds[1]);
    exit();
  }
  close(fds[1]);
  got = 0;
  while(read(fds[0], &c, 1) == 1){
    if(c == 'B')
      got = 1;
  }
  close(fds[0]);
  wait();
  check(got == 0, "writing past the break kills the process");
}

int
main(void)
{
  int before, after;

  printf(1, "lazytest: starting\n");
  before = freemem();

  sbrk_does_not_allocate_eagerly();
  touching_allocates_and_zeroes();
  kernel_writes_into_an_untouched_page();
  fork_carries_untouched_regions();
  access_past_the_break_kills_the_process();

  after = freemem();
  check(after >= before - 8, "no pages leaked overall");

  if(failures == 0)
    printf(1, "lazytest: OK\n");
  else
    printf(1, "lazytest: FAIL (%d)\n", failures);
  exit();
}

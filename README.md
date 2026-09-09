# Kernel Enhancements: Virtual Memory, Scheduling and Threads

Four substantial additions to a Unix-like x86 kernel, written in C: copy-on-write
fork with per-frame reference counting, a page fault handler that resolves both
copy-on-write and lazy-allocation faults, a multi-core preemptive scheduler with
per-CPU run queues and priority round-robin, and kernel threads with a userspace
threading library providing mutexes and condition variables.

**What this is built on.** The base is [xv6](https://github.com/mit-pdos/xv6-public)
(`xv6-public`, commit `eeb7b41`), MIT's teaching kernel for 32-bit x86 — a
re-implementation of Unix V6. It is Unix-like, **not Linux**: there is no Linux
source, no Linux API and no `task_struct` here. The first commit in this
repository is the unmodified upstream snapshot, so every line of the work below
is a reviewable diff against a known starting point:

```
git diff 064e750 HEAD -- '*.c' '*.h'      # 19 files, +1418 / -33
```

Everything below is verified by an automated suite that boots the kernel under
QEMU and asserts on real behaviour. Nothing here is claimed without a test.

## Quick start

The toolchain is pinned in a Dockerfile, so results do not depend on the host:

```bash
docker build -t xv6-dev .
docker run --rm -v "$PWD:/src" xv6-dev ./test/ci.sh all
```

To boot it interactively:

```bash
docker run --rm -it -v "$PWD:/src" xv6-dev make qemu-nox
```

## 1. Copy-on-write fork with per-frame reference counting

`fork` no longer copies user memory. `cowuvm` maps the parent's physical frames
into the child, clears `PTE_W` and sets a software `PTE_COW` bit on entries that
were writable, and takes a reference per mapping. The allocator (`kalloc.c`)
keeps a reference count per physical frame: `kalloc` sets it to one, `kfree`
decrements and only returns the frame to the free list at zero.

A write to a shared page traps to `cowfault`, which either restores write
permission in place when the frame has exactly one reference left, or allocates a
frame, copies, drops the old reference and remaps.

**Why the single-reference fast path is safe without extra locking:** a count of
one means no other page table maps that frame, so no other process is in a
position to raise the count. The observation cannot go stale.

**This also has to work for kernel writes.** `entry.S` sets `CR0_WP`, so a
supervisor write to a read-only page faults rather than silently bypassing the
protection. That is what makes a `read()` into a shared buffer safe, and
`cowtest` asserts exactly that: a child reading a file into a page it shares with
its parent must not be visible to the parent.

Measured by `cowtest`, using a `freemem` syscall that reports the real length of
the allocator's free list:

```
fork of 16 pages costs 70, fork of 256 pages costs 70
```

Fork cost is **independent of address space size**. The 70 pages are xv6's fixed
per-process cost for the child's kernel page tables. A copying fork would have
cost 240 pages more for the larger case.

## 2. Lazy allocation and the page fault handler

`sbrk` only moves the break; no physical page is allocated until first touch.
`T_PGFLT` gets its own case in `trap()`, which resolves a fault below the break
by allocating and mapping a zeroed page, then invalidating **that single virtual
address** with `invlpg` rather than reloading `cr3` and flushing the whole TLB.
`cowuvm` does reload `cr3`, because it changes many entries at once.

**Kernel-mode faults are handled too, and that is not optional here.** The x86
xv6 kernel runs on the faulting process's own page table and dereferences user
pointers directly — see `fetchint` and `fetchstr` in `syscall.c`. A `read()` into
a buffer the process has never touched therefore faults at CPL 0. A fault that
cannot be resolved is fatal to the process when it came from user mode, and
panics when it came from kernel mode, because that is a kernel bug rather than a
program error.

`lazytest` asserts that growing does not consume pages up front, that faulted
pages arrive zeroed, that the kernel can write into a page the process has never
touched, that a child's writes to untouched pages stay private, that shrinking
returns the pages, and that touching past the break kills that process and only
that process.

## 3. Multi-core preemptive scheduling

### First, the machine had to actually be multi-core

xv6 discovers processors by walking the legacy MP floating pointer table. Under
QEMU 6.2 that table describes **one** processor no matter what `-smp` says — the
config is byte-for-byte identical for `-smp 1`, `2` and `4` (208 bytes, one
`MPPROC` entry). Querying QEMU directly over QMP confirmed it really does create
the extra vCPUs:

```
query-cpus-fast -> 2 entries      # VCPU COUNT: 2
PROBE ncpu=1                      # what the kernel believed
```

Every "multi-core" run before this was uniprocessor. `acpi.c` replaces that
discovery: it locates the RSDP in the EBDA and BIOS area, validates checksums,
walks the RSDT, finds the MADT, and takes one CPU per enabled local APIC entry.
`mpinit` stays as a fallback.

One wrinkle: ACPI tables sit near the top of RAM — `0x1ffe1960` with `-m 512` —
far outside the 224 MB that xv6's direct map covers, so reading them through
`P2V` would fault. `kmap_extend` adds those pages to the kernel page table on
demand and is idempotent, so overlapping tables can each request their own range.

With that fixed, all four processors boot.

### The scheduler

Each CPU has its own run queue holding one FIFO list per priority level.
Selection takes the head of the highest non-empty level, so it is O(levels)
rather than a scan of all 64 process slots. A new process goes to the least
loaded queue; a process that yields returns to the queue it ran on, keeping its
cache warm; an idle CPU steals from the busiest queue that has more than one
process waiting.

Preemption is by quantum rather than on every tick, with higher priorities
getting longer quanta. Starvation is prevented by aging: a process that has
waited longer than `AGING_TICKS` is boosted a level, and its priority is restored
when it is next scheduled.

Measured by `schedtest`, oversubscribing the machine so priority actually
matters:

| machine | priority 0 work | priority 3 work | equal-priority spread |
|---|---:|---:|---|
| 2 CPUs, 4 spinners per level | 23316 | 2133 | 6946 – 7106 |
| 4 CPUs, 8 spinners per level | 37564 | 4333 | — |

High priority gets roughly 11x the CPU of low priority, low priority is still not
starved, and four equal-priority processes land within 2% of each other. Work is
observed running on every CPU (`mask 0xF` on four).

**An honest note on locking.** The run queues are protected by the existing
`ptable` lock, which already guards every `p->state` transition. That keeps
`sleep`, `wakeup`, `exit` and `wait` exactly as upstream wrote them. So the
queues buy O(1) selection, CPU affinity and explicit load balancing — but not
lock-free scalability. Splitting that lock means moving to per-process locks,
which is a much larger change and is deliberately not done here.

## 4. Kernel threads and a userspace threading library

`clone(fn, arg, stack)` creates a new kernel-scheduled thread that shares its
creator's address space, file table and working directory; `join(&stack)` waits
for one and hands back its stack. The address space is reference counted by
counting the processes using a given page directory, so it is freed only when the
last thread is reaped.

Blocking is done properly rather than by spinning. Two small syscalls,
`futex_wait(addr, expected)` and `futex_wake(addr, n)`, map onto the kernel's
`sleep`/`wakeup`, keyed on the **physical** address of the futex word.
`futex_wait` re-checks the value while holding the process table lock before
sleeping, which is what closes the lost-wakeup race.

`uthread.c` builds on those:

```c
void mutex_lock(struct mutex *m) {
  while(uxchg(&m->locked, 1) != 0)
    futex_wait((void*)&m->locked, 1);
}
```

Condition variables use the standard sequence-counter form, so a signal that
lands between the unlock and the wait is not lost.

Measured by `threadtest`:

```
counter reached 80000, expected 80000        # 4 threads x 20000 increments
produced 20100 consumed 20100 expected 20100 # bounded buffer, sum 1..200
threads observed on 2 cpus
```

## Testing

`test/xv6.exp` boots the kernel under QEMU with no display, waits for the shell
prompt before sending anything, runs commands and quits through the QEMU monitor.
Waiting for the prompt matters: piping into a fresh boot races with the console
and loses the first keystrokes.

A case fails on a harness timeout, on any occurrence of `panic` or an unexpected
trap in the transcript, on any line containing `FAIL`, or on a missing expected
marker.

```
usertests-2cpu               PASS      # the full upstream suite
usertests-1cpu               PASS
smoke                        PASS
lazy                         PASS      # 11 assertions
cow                          PASS      # 15 assertions
sched                        PASS      # 12 assertions
sched-4cpu                   PASS      # 12 assertions
thread                       PASS      # 11 assertions
```

That is 49 assertions of my own on top of the upstream suite.

The upstream `usertests` suite is the safety net for all of it, and it passes on
one, two and four CPUs.

Two harness problems in here were originally mistaken for kernel bugs, which is
worth recording: `usertests` refuses to run twice against the same filesystem
image, so every case now builds a fresh `fs.img`; and the one-CPU run was timing
out after 900 s in a disk-heavy test purely because the QEMU images sat on a
bind mount of the host filesystem. `test/ci.sh` copies the tree to
container-local storage first, after which the same kernel and the same test pass
in 69 seconds.

## What is deliberately not done

- **`fork` from a multithreaded process is rejected.** Marking the parent's pages
  read-only for copy-on-write while sibling threads run on other CPUs would leave
  stale writable TLB entries on those CPUs. Fixing it properly needs TLB
  shootdown via an IPI, which is not implemented, so the case returns an error
  rather than shipping a race.
- **`exec` from a thread is rejected** for the same address-space reason.
- The run queues share the global process table lock, as described above.
- Thread functions must call `exit()`; `clone` pushes a poison return address, so
  falling off the end of a thread function faults deliberately.
- Nested/repeated page-table structures, swapping and demand paging from disk are
  out of scope.

## Layout

| file | what changed |
|---|---|
| `kalloc.c` | per-frame reference counts, `freemem` |
| `vm.c` | `cowuvm`, `cowfault`, `lazyalloc`, `pagefault`, `kmap_extend`, `uva2pa` |
| `trap.c` | `T_PGFLT` handling, quantum-based preemption |
| `acpi.c`, `acpi.h` | ACPI RSDP/RSDT/MADT parsing for CPU discovery |
| `proc.c` | per-CPU run queues, priorities, aging, `clone`, `join`, futexes |
| `uthread.c`, `uthread.h` | userspace threads, mutexes, condition variables |
| `lazytest.c`, `cowtest.c`, `schedtest.c`, `threadtest.c` | the assertions above |
| `test/` | QEMU harness, runner, container entry point |

Nine syscalls were added: `freemem`, `setpriority`, `getpriority`, `getcpu`,
`getncpu`, `clone`, `join`, `futex_wait`, `futex_wake`.

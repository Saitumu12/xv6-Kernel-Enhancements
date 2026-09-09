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

```bash
git diff 064e750 HEAD -- '*.c' '*.h'
```

Everything below is verified by an automated suite that boots the kernel under
QEMU and asserts on real behaviour. Nothing here is claimed without a test, and
no figures are quoted that you would have to take on trust — the suite prints its
own results on your machine.

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

A write to such a page traps to `cowfault`, which either restores write
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

**The claim that fork stops copying is measured, not assumed.** A `freemem`
syscall reports the real length of the allocator's free list, and `cowtest` forks
twice — once with a small heap, once with a much larger one — and compares the
physical pages consumed by each. Under a copying fork the larger case costs
proportionally more; under copy-on-write both cost the same, and what remains is
xv6's fixed per-process cost for the child's kernel page tables. The test prints
both figures and asserts that the difference does not grow with the size of the
address space.

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
the QEMU version pinned in the Dockerfile, that table describes a **single**
processor no matter what `-smp` says — the config it hands back is byte-for-byte
identical whether you ask for one CPU or several. Querying QEMU directly over QMP
confirmed it really does create the extra vCPUs, so the kernel was the one at
fault, and every run before this had been uniprocessor while claiming otherwise.

`acpi.c` replaces that discovery: it locates the RSDP in the EBDA and BIOS area,
validates checksums, walks the RSDT, finds the MADT, and takes one CPU per
enabled local APIC entry. `mpinit` stays as a fallback when no usable ACPI table
is present.

One wrinkle: the ACPI tables sit near the top of RAM, outside the region xv6's
direct map covers (`PHYSTOP`), so reading them through `P2V` would fault.
`kmap_extend` adds those pages to the kernel page table on demand and is
idempotent, so overlapping tables can each request their own range.

With that fixed, every processor QEMU is asked for boots and prints its startup
line.

### The scheduler

Each CPU has its own run queue holding one FIFO list per priority level.
Selection takes the head of the highest non-empty level, so it is O(levels)
rather than a scan of the whole process table. A new process goes to the least
loaded queue; a process that yields returns to the queue it ran on, keeping its
cache warm; an idle CPU steals from the busiest queue that has more than one
process waiting.

Preemption is by quantum rather than on every tick, with higher priorities
getting longer quanta. Starvation is prevented by aging: a process that has
waited longer than `AGING_TICKS` is boosted a level, and its priority is restored
when it is next scheduled.

`schedtest` oversubscribes the machine — more spinners than CPUs, so priority
actually matters — and then measures. Each child busy-works until a shared
deadline and reports how much it completed, so the comparison is of real CPU time
received rather than of anything the scheduler claims about itself. It asserts
that a high-priority group completes substantially more work than a low-priority
group, that the low-priority group is nonetheless **not starved**, that equal
priority processes finish within a small factor of each other, and that work is
observed running on more than one CPU. Every process also records which CPUs it
actually ran on, sampled during its work rather than once at the end, since where
a process happens to finish says little about where its work went.

Run it with `./test/ci.sh sched sched4` to see the figures for your machine.

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

`threadtest` asserts exact outcomes rather than approximate ones: several threads
each performing a fixed number of increments under a mutex must leave the counter
at exactly the expected total, and a bounded buffer driven by condition variables
must transfer every item exactly once, checked by comparing the produced and
consumed sums against the arithmetic total. It also asserts that a broadcast
releases every waiter, that threads of one process are observed on more than one
CPU, and that `join` with nothing outstanding reports an error.

## Testing

`test/xv6.exp` boots the kernel under QEMU with no display, waits for the shell
prompt before sending anything, runs commands and quits through the QEMU monitor.
Waiting for the prompt matters: piping into a fresh boot races with the console
and loses the first keystrokes.

A case fails on a harness timeout, on any occurrence of `panic` or an unexpected
trap in the transcript, on any line containing `FAIL`, or on a missing expected
marker.

```
usertests-2cpu      the full upstream suite, two CPUs
usertests-1cpu      the full upstream suite, one CPU
smoke               boots and runs a command
lazy                lazy allocation and fault handling
cow                 copy-on-write and reference counting
sched               scheduler policy, two CPUs
sched-4cpu          scheduler policy, four CPUs
thread              threads, mutexes, condition variables
stress              filesystem and fork stress
```

All of them pass, on a fresh clone, with `./test/ci.sh all`. The upstream
`usertests` suite is the safety net for all of it, and it passes on one, two and
four CPUs.

Two harness problems in here were originally mistaken for kernel bugs, which is
worth recording: `usertests` refuses to run twice against the same filesystem
image, so every case now builds a fresh `fs.img`; and the one-CPU run was timing
out in a disk-heavy test purely because the QEMU images sat on a bind mount of
the host filesystem. `test/ci.sh` copies the tree to container-local storage
first, after which the same kernel and the same test pass comfortably.

Two genuine kernel bugs turned up while building this — the first by reading the
code back, the second because the upstream suite started hanging. Neither had a
test that would have caught it, so both got one afterwards:

- Threads sharing an address space could fault on the same page simultaneously.
  The copy-on-write path resolved it for the first thread and returned an error
  to the second, killing the process, and two simultaneous lazy faults on one
  address would have reached `mappages` twice and panicked on remap. Fault
  resolution now happens under a single VM lock, and a fault another thread has
  already resolved is reported as handled rather than fatal.
- `allocproc` never cleared `p->pgdir`, so a recycled slot in `EMBRYO` still
  pointed at a page directory a concurrent `wait()` was about to free. That stale
  pointer made the directory look shared, `wait()` skipped `freevm`, and an
  address space leaked on every fork — which under a tight fork/exit loop
  exhausted memory and looked exactly like a hang.

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

Syscalls added: `freemem`, `setpriority`, `getpriority`, `getcpu`, `getncpu`,
`clone`, `join`, `futex_wait`, `futex_wake`.

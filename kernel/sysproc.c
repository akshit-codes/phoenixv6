#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "fs.h"
#include "undo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Fetch the nth system call argument as a snapshot name:
// 1 to SNAMELEN-1 letters or digits. The name becomes part of a
// file name, so nothing else is allowed.
static int
argsname(int n, char *name)
{
  int i;

  if (argstr(n, name, SNAMELEN) < 1)
    return -1;
  for (i = 0; name[i]; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9')))
      return -1;
  }
  return 0;
}

// checkpoint(pid, name): ask process pid to save a snapshot of
// itself. Returns as soon as the request is recorded.
uint64
sys_checkpoint(void)
{
  int pid;
  char name[SNAMELEN];

  argint(0, &pid);
  if (argsname(1, name) < 0)
    return -1;
  return snap_request(pid, name);
}

// restore(name): start a new process from a snapshot.
// Returns the new process's pid.
uint64
sys_restore(void)
{
  char name[SNAMELEN];

  if (argsname(0, name) < 0)
    return -1;
  return snap_restore(name);
}

// autosave(pid, ticks, name): have process pid save a snapshot of
// itself every ticks timer ticks. ticks == 0 turns it off, and the
// name is then ignored.
uint64
sys_autosave(void)
{
  int pid, interval;
  char name[SNAMELEN];

  argint(0, &pid);
  argint(1, &interval);
  if (interval < 0)
    return -1;
  name[0] = 0;
  if (interval > 0 && argsname(2, name) < 0)
    return -1;
  return snap_autosave(pid, interval, name);
}

// undo(op): start journaling the caller's file changes (UNDO_BEGIN),
// or stop and keep them (UNDO_COMMIT) or take them back (UNDO_ABORT).
uint64
sys_undo(void)
{
  int op;

  argint(0, &op);
  switch (op) {
  case UNDO_BEGIN:
    return undo_begin();
  case UNDO_COMMIT:
    return undo_end(0);
  case UNDO_ABORT:
    return undo_end(1);
  }
  return -1;
}

//
// AutoSave: save a running process to a file, and build a new
// process from such a file.
//
// A process is saved by itself, from usertrap(), on its way back to
// user space. At that point its trapframe holds its complete user
// CPU state and its user memory cannot change, so the two describe
// the same instant. Another process only asks for a snapshot
// (snap_request() and snap_autosave() in proc.c); it never reads the
// target's memory.
//
// Restoring is fork() with a file in place of the parent: where
// kfork() copies memory, registers and open files from the parent,
// snap_restore() reads them from the snapshot.
//
// A save is too big for one log transaction, so the log alone cannot
// make it atomic. Instead each name has two snapshot files (slots).
// A save writes into the slot that does not hold the newest complete
// snapshot, and only a final one-block write of the header makes the
// new one count. A crash at any moment leaves at least the previous
// snapshot intact.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "snap.h"

extern struct spinlock wait_lock; // proc.c

// The header must fit in the block reserved for it.
_Static_assert(sizeof(struct snaphdr) <= SNAP_DATA, "snaphdr too big");

#define NSLOT      2
#define CKSUM_INIT 2166136261u

// Path of a snapshot slot: "/<name>.s<slot>".
static void
slotpath(char *path, char *name, int slot)
{
  int n = strlen(name);

  path[0] = '/';
  memmove(path + 1, name, n);
  path[n + 1] = '.';
  path[n + 2] = 's';
  path[n + 3] = '0' + slot;
  path[n + 4] = 0;
}

// Write n bytes from kernel address src to ip at offset off.
// Like filewrite(), it writes a few blocks per transaction so as
// not to overflow the log. ip must be referenced but not locked.
static int
slot_write(struct inode *ip, uint off, char *src, uint n)
{
  uint max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  uint done = 0;

  while (done < n) {
    uint m = n - done;
    if (m > max)
      m = max;
    begin_op();
    ilock(ip);
    int r = writei(ip, 0, (uint64)src + done, off + done, m);
    iunlock(ip);
    end_op();
    if (r != m)
      return -1;
    done += m;
  }
  return 0;
}

// Read exactly n bytes from ip at offset off to kernel address dst.
static int
slot_read(struct inode *ip, uint off, char *dst, uint n)
{
  ilock(ip);
  int r = readi(ip, 0, (uint64)dst, off, n);
  iunlock(ip);
  return r == n ? 0 : -1;
}

// Drop a reference to an inode. iput() may free a file, so it has
// to run inside a transaction.
static void
slot_close(struct inode *ip)
{
  begin_op();
  iput(ip);
  end_op();
}

// Open a slot and read its header into h, which must be SNAP_DATA
// bytes. Returns the slot's inode, referenced and unlocked, if the
// slot holds a complete snapshot; otherwise 0.
static struct inode *
slot_open(char *name, int slot, struct snaphdr *h)
{
  char path[SNAMELEN + 5];
  struct inode *ip;

  slotpath(path, name, slot);
  begin_op();
  ip = namei(path);
  end_op();
  if (ip == 0)
    return 0;
  if (slot_read(ip, 0, (char *)h, SNAP_DATA) == 0 && h->magic == SNAP_MAGIC &&
      h->valid == 1 && h->generation > 0 && h->sz <= TRAPFRAME)
    return ip;
  slot_close(ip);
  return 0;
}

// The generation of the snapshot in a slot, or 0 if the slot has no
// complete snapshot. h is scratch space of SNAP_DATA bytes.
static uint
slot_generation(char *name, int slot, struct snaphdr *h)
{
  struct inode *ip;

  if ((ip = slot_open(name, slot, h)) == 0)
    return 0;
  slot_close(ip);
  return h->generation;
}

// Continue a checksum (32-bit FNV-1a) over n more bytes.
static uint
cksum(uint sum, char *p, uint n)
{
  for (uint i = 0; i < n; i++) {
    sum ^= (uchar)p[i];
    sum *= 16777619;
  }
  return sum;
}

// Is it time for p to save a snapshot of itself?
// Called by p itself, from usertrap().
int
snap_due(struct proc *p)
{
  int due;

  acquire(&p->lock);
  // ticks is read without tickslock: being a tick early or late
  // does not matter here. The subtraction keeps the comparison
  // right if ticks ever wraps around.
  due = p->snap_req || (p->as_interval > 0 && (int)(ticks - p->as_next) >= 0);
  release(&p->lock);
  return due;
}

// Stop autosaving p: it cannot be saved.
static void
snap_giveup(struct proc *p)
{
  acquire(&p->lock);
  p->as_interval = 0;
  release(&p->lock);
}

// Save the calling process p to one of its snapshot files.
// Called from usertrap() just before p returns to user space.
void
snap_save(struct proc *p)
{
  char name[SNAMELEN], path[SNAMELEN + 5];
  struct snaphdr *h = 0;
  struct inode *ip = 0;
  struct snappage rec;
  pte_t *pte;
  uint64 va;
  uint off, npages, sum, gen[NSLOT], now;
  int fd, i, slot, asked, interval;

  // Take the request, and book the next periodic save now, so that
  // a save that fails is not retried on every trap.
  acquire(&p->lock);
  safestrcpy(name, p->sname, sizeof(name));
  asked = p->snap_req;
  interval = p->as_interval;
  now = ticks;
  p->snap_req = 0;
  p->as_next = now + interval;
  release(&p->lock);

  // Writing to the disk sleeps, so interrupts must be on. That is
  // safe here for the same reason as in the system call path of
  // usertrap(): the user pc is already saved in the trapframe.
  intr_on();

  // A pipe's contents live in kernel memory shared with another
  // process; they cannot be saved.
  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] && p->ofile[fd]->type == FD_PIPE) {
      printk("snapshot %s: pid %d has a pipe open; not saved\n", name, p->pid);
      snap_giveup(p);
      return;
    }
  }

  // Count the pages that are really there. With lazy allocation,
  // addresses below p->sz may have no page yet.
  npages = 0;
  for (va = 0; va < p->sz; va += PGSIZE) {
    if ((pte = walk(p->pagetable, va, 0)) != 0 && (*pte & PTE_V))
      npages++;
  }
  if (SNAP_DATA + npages * (sizeof(rec) + PGSIZE) > MAXFILE * BSIZE) {
    printk("snapshot %s: pid %d is too big (%d pages); not saved\n", name,
           p->pid, npages);
    snap_giveup(p);
    return;
  }

  // The header gets a page of its own; it is too big for the
  // kernel stack.
  if ((h = (struct snaphdr *)kalloc()) == 0)
    goto fail;

  // Choose the slot to overwrite: the one that does NOT hold the
  // newest complete snapshot. The new snapshot's generation is one
  // more than the newest.
  for (i = 0; i < NSLOT; i++)
    gen[i] = slot_generation(name, i, h);
  slot = gen[0] <= gen[1] ? 0 : 1;

  memset(h, 0, PGSIZE);
  h->magic = SNAP_MAGIC;
  h->valid = 0;
  h->generation = (gen[0] > gen[1] ? gen[0] : gen[1]) + 1;
  h->npages = npages;
  h->saved_at = now;
  h->interval = interval;
  h->cwd_inum = p->cwd->inum;
  h->sz = p->sz;
  safestrcpy(h->name, p->name, sizeof(h->name));
  h->tf = *(p->trapframe);
  for (fd = 0; fd < NOFILE; fd++) {
    struct file *f = p->ofile[fd];
    struct snapfile *sf = &h->files[fd];

    sf->dupof = -1;
    if (f == 0)
      continue; // sf->type is FD_NONE
    // After dup() or fork(), descriptors share one struct file and
    // so one offset. Record that, to recreate the sharing.
    for (i = 0; i < fd; i++) {
      if (p->ofile[i] == f) {
        sf->dupof = i;
        break;
      }
    }
    sf->type = f->type;
    sf->readable = f->readable;
    sf->writable = f->writable;
    if (f->type == FD_INODE) {
      sf->inum = f->ip->inum;
      sf->off = f->off;
    } else {
      sf->major = f->major;
    }
  }

  // Open the slot's file, creating it if need be.
  slotpath(path, name, slot);
  begin_op();
  ip = create(path, T_FILE, 0, 0); // returns ip locked
  if (ip == 0) {
    end_op();
    goto fail;
  }
  iunlock(ip);
  end_op();

  // 1. Mark the slot invalid. It may hold an older snapshot, and
  //    after a crash nobody must mistake a mixture of old and new
  //    pages for one.
  if (slot_write(ip, 0, (char *)h, SNAP_DATA) < 0)
    goto fail;

  // 2. Write every page that is mapped: its address, its permission
  //    bits and its contents. This is the loop of uvmcopy(), writing
  //    to a file instead of to a new page. The kernel maps all of
  //    physical memory, so PTE2PA() gives a usable pointer.
  off = SNAP_DATA;
  sum = CKSUM_INIT;
  for (va = 0; va < p->sz; va += PGSIZE) {
    if ((pte = walk(p->pagetable, va, 0)) == 0)
      continue;
    if ((*pte & PTE_V) == 0)
      continue;
    rec.va = va;
    rec.flags = PTE_FLAGS(*pte);
    if (slot_write(ip, off, (char *)&rec, sizeof(rec)) < 0)
      goto fail;
    if (slot_write(ip, off + sizeof(rec), (char *)PTE2PA(*pte), PGSIZE) < 0)
      goto fail;
    sum = cksum(sum, (char *)&rec, sizeof(rec));
    sum = cksum(sum, (char *)PTE2PA(*pte), PGSIZE);
    off += sizeof(rec) + PGSIZE;
  }

  // 3. Commit. The header is one block, so this write is a single
  //    log transaction: after a crash it has either happened or not.
  //    Until it happens, the other slot is still the newest.
  h->valid = 1;
  h->checksum = sum;
  if (slot_write(ip, 0, (char *)h, SNAP_DATA) < 0)
    goto fail;

  if (asked)
    printk("snapshot %s: saved pid %d (%d pages, generation %d)\n", name,
           p->pid, npages, h->generation);
  goto out;

fail:
  printk("snapshot %s: could not save pid %d\n", name, p->pid);
out:
  if (ip)
    slot_close(ip);
  if (h)
    kfree((void *)h);
}

// Recreate one open file from its description.
static struct file *
snap_reopen(struct snapfile *sf)
{
  struct inode *ip = 0;
  struct file *f;

  begin_op();
  if (sf->type == FD_INODE) {
    // An inode does not know its path, so the snapshot records the
    // inode number. The file may have been deleted since.
    ip = iget_type(ROOTDEV, sf->inum, T_FILE);
  } else if (sf->type == FD_DEVICE && sf->major >= 0 && sf->major < NDEV) {
    ip = namei("/console");
  }
  if (ip == 0) {
    end_op();
    return 0;
  }
  if ((f = filealloc()) == 0) {
    iput(ip);
    end_op();
    return 0;
  }
  end_op();

  f->type = sf->type;
  f->ip = ip; // the file now owns the reference to ip
  f->readable = sf->readable;
  f->writable = sf->writable;
  f->off = sf->type == FD_INODE ? sf->off : 0;
  f->major = sf->major;
  return f;
}

// Build a process from the snapshot in ip, whose header is h.
// Returns the new pid, or -1 if the snapshot is unusable.
static int
snap_build(struct inode *ip, struct snaphdr *h, char *name)
{
  struct proc *np;
  struct proc *p = myproc();
  struct snappage rec;
  uint64 last = 0;
  uint off, perm, sum;
  char *mem;
  int i, fd, pid;

  // From here on this follows kfork().
  if ((np = allocproc()) == 0)
    return -1;
  // allocproc() returns holding np->lock, a spinlock, but reading
  // the snapshot sleeps. Let go of it: np is in state USED, which
  // keeps the scheduler and other allocproc() callers away from it.
  release(&np->lock);
  np->sz = h->sz;

  // User memory: the second half of uvmcopy()'s loop, with the
  // file as the source.
  off = SNAP_DATA;
  sum = CKSUM_INIT;
  for (i = 0; i < h->npages; i++) {
    if (slot_read(ip, off, (char *)&rec, sizeof(rec)) < 0)
      goto bad;
    // Don't trust the file: mappages() panics on a bad or repeated
    // address, and a PTE without R, W or X would be taken for a
    // pointer to another page-table page.
    perm = rec.flags & (PTE_R | PTE_W | PTE_X | PTE_U);
    if (rec.va % PGSIZE != 0 || rec.va >= h->sz || (i > 0 && rec.va <= last) ||
        (perm & (PTE_R | PTE_W | PTE_X)) == 0)
      goto bad;
    if ((mem = kalloc()) == 0)
      goto bad;
    if (slot_read(ip, off + sizeof(rec), mem, PGSIZE) < 0 ||
        mappages(np->pagetable, rec.va, PGSIZE, (uint64)mem, perm) != 0) {
      kfree(mem);
      goto bad;
    }
    sum = cksum(sum, (char *)&rec, sizeof(rec));
    sum = cksum(sum, mem, PGSIZE);
    last = rec.va;
    off += sizeof(rec) + PGSIZE;
  }
  // Every page must be exactly what was saved.
  if (sum != h->checksum)
    goto bad;

  // User registers. Unlike kfork() there is no return value to
  // patch: the process resumes with exactly the registers it had.
  *(np->trapframe) = h->tf;

  h->name[sizeof(h->name) - 1] = 0;
  safestrcpy(np->name, h->name, sizeof(np->name));

  // Open files.
  for (fd = 0; fd < NOFILE; fd++) {
    struct snapfile *sf = &h->files[fd];

    if (sf->type == FD_NONE)
      continue;
    if (sf->dupof >= 0 && sf->dupof < fd && np->ofile[sf->dupof]) {
      np->ofile[fd] = filedup(np->ofile[sf->dupof]);
      continue;
    }
    if ((np->ofile[fd] = snap_reopen(sf)) == 0)
      goto bad;
  }

  // Current directory; fall back to the root if it is gone.
  begin_op();
  if ((np->cwd = iget_type(ROOTDEV, h->cwd_inum, T_DIR)) == 0)
    np->cwd = namei("/");
  end_op();

  pid = np->pid;

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  // Carry on autosaving under the same name, if it was being done.
  safestrcpy(np->sname, name, sizeof(np->sname));
  np->as_interval = h->interval > 0 ? h->interval : 0;
  np->as_next = ticks + np->as_interval;
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;

bad:
  for (fd = 0; fd < NOFILE; fd++) {
    if (np->ofile[fd]) {
      fileclose(np->ofile[fd]);
      np->ofile[fd] = 0;
    }
  }
  if (np->cwd) {
    begin_op();
    iput(np->cwd);
    end_op();
    np->cwd = 0;
  }
  acquire(&np->lock);
  freeproc(np); // frees the pages mapped so far, too
  release(&np->lock);
  return -1;
}

// Create a new process from the newest usable snapshot called name.
// The new process is a child of the caller and continues from the
// instant the snapshot was taken. Returns its pid, or -1.
int
snap_restore(char *name)
{
  struct snaphdr *h;
  struct inode *ip;
  uint gen[NSLOT];
  int i, slot, pid = -1;

  if ((h = (struct snaphdr *)kalloc()) == 0)
    return -1;

  for (i = 0; i < NSLOT; i++)
    gen[i] = slot_generation(name, i, h);

  // Try the newer slot first, then the older one.
  slot = gen[1] > gen[0] ? 1 : 0;
  for (i = 0; i < NSLOT && pid < 0; i++, slot = 1 - slot) {
    if (gen[slot] == 0)
      continue;
    if ((ip = slot_open(name, slot, h)) == 0)
      continue;
    pid = snap_build(ip, h, name);
    if (pid < 0)
      printk("restore %s: slot %d (generation %d) is unusable\n", name, slot,
             gen[slot]);
    slot_close(ip);
  }

  kfree((void *)h);
  return pid;
}

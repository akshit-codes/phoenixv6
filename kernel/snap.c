//
// AutoSave: save a running process to a file, and build a new
// process from such a file.
//
// A process is saved by itself, from usertrap(), on its way back to
// user space. At that point its trapframe holds its complete user
// CPU state and its user memory cannot change, so the two describe
// the same instant. Another process only asks for a snapshot
// (snap_request() in proc.c); it never reads the target's memory.
//
// Restoring is fork() with a file in place of the parent: where
// kfork() copies memory, registers and open files from the parent,
// snap_restore() reads them from the snapshot.
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

// Has a snapshot of p been asked for?
// Called by p itself, from usertrap().
int
snap_due(struct proc *p)
{
  int due;

  acquire(&p->lock);
  due = p->snap_req;
  release(&p->lock);
  return due;
}

// Save the calling process p to its snapshot file.
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
  uint off, npages;
  int fd, i;

  acquire(&p->lock);
  safestrcpy(name, p->sname, sizeof(name));
  p->snap_req = 0;
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
    return;
  }

  // Build the header in a page of its own; it is too big for the
  // kernel stack.
  if ((h = (struct snaphdr *)kalloc()) == 0)
    goto fail;
  memset(h, 0, PGSIZE);
  h->magic = SNAP_MAGIC;
  h->valid = 0;
  h->npages = npages;
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

  // Open the snapshot file, creating it if need be.
  slotpath(path, name, 0);
  begin_op();
  ip = create(path, T_FILE, 0, 0); // returns ip locked
  if (ip == 0) {
    end_op();
    goto fail;
  }
  iunlock(ip);
  end_op();

  // 1. Mark the file invalid. If we crash while writing pages over
  //    an older snapshot, nobody will mistake the mixture for one.
  if (slot_write(ip, 0, (char *)h, SNAP_DATA) < 0)
    goto fail;

  // 2. Write every page that is mapped: its address, its permission
  //    bits and its contents. This is the loop of uvmcopy(), writing
  //    to a file instead of to a new page. The kernel maps all of
  //    physical memory, so PTE2PA() gives a usable pointer.
  off = SNAP_DATA;
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
    off += sizeof(rec) + PGSIZE;
  }

  // 3. Commit: one small write makes the snapshot valid.
  h->valid = 1;
  if (slot_write(ip, 0, (char *)h, SNAP_DATA) < 0)
    goto fail;

  printk("snapshot %s: saved pid %d (%d pages)\n", name, p->pid, npages);
  goto out;

fail:
  printk("snapshot %s: could not save pid %d\n", name, p->pid);
out:
  if (ip) {
    begin_op();
    iput(ip);
    end_op();
  }
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

// Create a new process from the snapshot called name. The new
// process is a child of the caller and continues from the instant
// the snapshot was taken. Returns its pid, or -1.
int
snap_restore(char *name)
{
  char path[SNAMELEN + 5];
  struct snaphdr *h = 0;
  struct inode *ip;
  struct proc *np = 0;
  struct proc *p = myproc();
  struct snappage rec;
  uint64 last = 0;
  uint off, perm;
  char *mem;
  int i, fd, pid = -1;

  slotpath(path, name, 0);
  begin_op();
  ip = namei(path);
  end_op();
  if (ip == 0)
    return -1;

  if ((h = (struct snaphdr *)kalloc()) == 0)
    goto bad;
  if (slot_read(ip, 0, (char *)h, SNAP_DATA) < 0)
    goto bad;
  if (h->magic != SNAP_MAGIC || h->valid != 1 || h->sz > TRAPFRAME)
    goto bad;

  // From here on this follows kfork().
  if ((np = allocproc()) == 0)
    goto bad;
  // allocproc() returns holding np->lock, a spinlock, but reading
  // the snapshot sleeps. Let go of it: np is in state USED, which
  // keeps the scheduler and other allocproc() callers away from it.
  release(&np->lock);
  np->sz = h->sz;

  // User memory: the second half of uvmcopy()'s loop, with the
  // file as the source.
  off = SNAP_DATA;
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
    last = rec.va;
    off += sizeof(rec) + PGSIZE;
  }

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
  np->state = RUNNABLE;
  release(&np->lock);

  goto out;

bad:
  if (np) {
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
  }
out:
  begin_op();
  iput(ip);
  end_op();
  if (h)
    kfree((void *)h);
  return pid;
}

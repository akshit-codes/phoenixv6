//
// Undo: a journal of the file changes a process makes, so that they
// can all be taken back.
//
// A process with p->undo_on set (and every child it forks) has its
// changes journaled under its session name p->sname, in a directory
// of its own:
//
//   /<name>.u/journal   a header and a list of struct urec, in the
//                       order the changes were made
//   /<name>.u/b<inum>   the contents file <inum> had before its
//                       first change
//   /<name>.u/d<inum>   an extra link to a deleted file, which keeps
//                       its contents from being freed
//
// The xv6 log (log.c) is a redo log: it records new contents and
// replays them forward to finish an operation. This is an undo log:
// it records old contents and replays them backward to cancel one.
//
// The rule that makes it safe: what is needed to undo a change is
// written before the change is made. Log transactions reach the
// disk in the order they end, so after a crash the journal can
// describe a change that never happened (harmless: undoing it
// changes nothing), but never miss one that did.
//
// Every step of a rollback can be repeated without harm, and the
// journal is only emptied after the last one. So a crash during a
// rollback is cured by running it again.
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
#include "fcntl.h"
#include "undo.h"

// One lock for all journaling. It is a sleeplock because journaling
// does disk I/O, and it is always taken before begin_op(), never
// inside a transaction, so it cannot deadlock against the log.
struct sleeplock undolock;

void
undoinit(void)
{
  initsleeplock(&undolock, "undo");
}

// path = "/<name>.u" if ent is 0, else "/<name>.u/<ent>".
static void
upath(char *path, char *name, char *ent)
{
  int n = strlen(name);

  path[0] = '/';
  memmove(path + 1, name, n);
  path[n + 1] = '.';
  path[n + 2] = 'u';
  path[n + 3] = 0;
  if (ent) {
    path[n + 3] = '/';
    safestrcpy(path + n + 4, ent, DIRSIZ + 1);
  }
}

// ent = prefix followed by inum in decimal, e.g. "b23".
static void
entname(char *ent, char prefix, uint inum)
{
  char tmp[12];
  int t = 0, n = 0;

  do {
    tmp[t++] = '0' + inum % 10;
    inum /= 10;
  } while (inum);
  ent[n++] = prefix;
  while (t > 0)
    ent[n++] = tmp[--t];
  ent[n] = 0;
}

// The calling process's session name.
static void
myname(char *name)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  safestrcpy(name, p->sname, SNAMELEN);
  release(&p->lock);
}

// The session's undo directory, referenced and unlocked, or 0.
// If make is set, it is created when missing.
static struct inode *
undo_dir(char *name, int make)
{
  char path[UPATH];
  struct inode *dp;

  upath(path, name, 0);
  begin_op();
  dp = namei(path);
  if (dp == 0 && make) {
    if ((dp = create(path, T_DIR, 0, 0)) != 0) // returns dp locked
      iunlock(dp);
  }
  end_op();
  return dp;
}

// The file ent in the session's undo directory, referenced and
// unlocked, or 0. If make is set, it is created when missing.
static struct inode *
undo_open(char *name, char *ent, int make)
{
  char path[UPATH];
  struct inode *ip;

  upath(path, name, ent);
  begin_op();
  if (make) {
    if ((ip = create(path, T_FILE, 0, 0)) != 0) // returns ip locked
      iunlock(ip);
  } else {
    ip = namei(path);
  }
  end_op();
  return ip;
}

// Remove ent from the session's undo directory.
static void
undo_remove(char *name, char *ent)
{
  struct inode *dp;

  if ((dp = undo_dir(name, 0)) == 0)
    return;
  begin_op();
  ilock(dp);
  dirunlink(dp, ent);
  iunlockput(dp);
  end_op();
}

// Copy all of src to dst, from offset 0. Both are referenced and
// unlocked. One inode is locked at a time, and each chunk written
// is a transaction of its own.
static int
undo_copy(struct inode *dst, struct inode *src)
{
  uint max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  uint off = 0;
  char *buf;
  int n, r = 0;

  if ((buf = kalloc()) == 0)
    return -1;
  for (;;) {
    ilock(src);
    n = readi(src, 0, (uint64)buf, off, max);
    iunlock(src);
    if (n <= 0) {
      r = n;
      break;
    }
    if (slot_write(dst, off, buf, n) < 0) {
      r = -1;
      break;
    }
    off += n;
  }
  kfree(buf);
  return r;
}

// Add a record to the end of the session's journal.
static int
undo_append(char *name, struct urec *r)
{
  struct inode *jp;
  struct ujhdr hdr;
  uint size;
  int ret = -1;

  if ((jp = undo_open(name, "journal", 1)) == 0)
    return -1;
  ilock(jp);
  size = jp->size;
  iunlock(jp);
  if (size < sizeof(hdr)) {
    // A new journal starts with its header.
    hdr.magic = UNDO_MAGIC;
    hdr.epoch = 0;
    if (slot_write(jp, 0, (char *)&hdr, sizeof(hdr)) < 0)
      goto out;
    size = sizeof(hdr);
  }
  ret = slot_write(jp, size, (char *)r, sizeof(*r));
out:
  slot_close(jp);
  return ret;
}

// Journal a record for the calling process. Makes the undo
// directory if this is the session's first record.
static int
undo_record(struct urec *r)
{
  char name[SNAMELEN];
  struct inode *dp;
  int ret = -1;

  myname(name);
  acquiresleep(&undolock);
  if ((dp = undo_dir(name, 1)) != 0) {
    slot_close(dp);
    ret = undo_append(name, r);
  }
  releasesleep(&undolock);
  return ret;
}

//
// The hooks. Each is called, for a process with undo_on set, before
// the change it protects, with no transaction open and no inode
// locked.
//

// The calling process is about to change the contents of ip
// (referenced, not locked). The first time that happens to a file,
// save a copy of it. Returns -1 if the copy could not be made, in
// which case the change must not go ahead.
int
undo_before_write(struct inode *ip)
{
  char name[SNAMELEN], ent[DIRSIZ];
  struct inode *dp, *bp;
  struct urec r;
  uint size;
  short type;
  int ret = -1;

  ilock(ip);
  type = ip->type;
  size = ip->size;
  iunlock(ip);
  if (type != T_FILE)
    return 0;

  myname(name);
  entname(ent, 'b', ip->inum);

  acquiresleep(&undolock);
  if ((dp = undo_dir(name, 1)) == 0)
    goto out;
  slot_close(dp);

  // A backup that exists is complete (it was made under undolock)
  // and older than anything we could save now.
  if ((bp = undo_open(name, ent, 0)) != 0) {
    slot_close(bp);
    ret = 0;
    goto out;
  }

  if ((bp = undo_open(name, ent, 1)) == 0)
    goto out;
  if (undo_copy(bp, ip) == 0) {
    memset(&r, 0, sizeof(r));
    r.op = U_MODIFY;
    r.inum = ip->inum;
    r.oldsize = size;
    ret = undo_append(name, &r);
  }
  slot_close(bp);
  if (ret < 0)
    undo_remove(name, ent); // don't leave half a backup behind

out:
  releasesleep(&undolock);
  return ret;
}

// The calling process is about to open path with omode. If that
// will create the file, journal the creation; if it will truncate
// an existing file, save the file first.
int
undo_before_open(char *path, int omode)
{
  char ent[DIRSIZ];
  struct inode *ip, *dp;
  struct urec r;
  int ret = 0;

  begin_op();
  ip = namei(path);
  end_op();

  if (ip != 0) {
    // It exists: only truncation destroys anything.
    if (omode & O_TRUNC)
      ret = undo_before_write(ip);
    slot_close(ip);
    return ret;
  }
  if ((omode & O_CREATE) == 0)
    return 0; // the open will simply fail

  // It will be created. Remember where, so that it can be removed.
  begin_op();
  dp = nameiparent(path, ent);
  if (dp == 0) {
    end_op();
    return 0; // no such directory: the open will fail
  }
  memset(&r, 0, sizeof(r));
  r.op = U_CREATE;
  r.pinum = dp->inum;
  strncpy(r.name, ent, DIRSIZ);
  iput(dp);
  end_op();
  return undo_record(&r);
}

// The calling process is about to unlink path. Journal that, and
// return the session's undo directory (referenced, not locked) for
// undo_keep() to use, or 0 if there is nothing to keep.
struct inode *
undo_before_unlink(char *path)
{
  char name[SNAMELEN], ent[DIRSIZ];
  struct inode *ip, *dp;
  struct urec r;
  short type;

  begin_op();
  if ((dp = nameiparent(path, ent)) == 0) {
    end_op();
    return 0;
  }
  ilock(dp);
  ip = dirlookup(dp, ent, 0);
  iunlock(dp);
  if (ip == 0) {
    iput(dp);
    end_op();
    return 0;
  }
  ilock(ip);
  type = ip->type;
  iunlock(ip);

  memset(&r, 0, sizeof(r));
  r.op = U_DELETE;
  r.inum = ip->inum;
  r.pinum = dp->inum;
  strncpy(r.name, ent, DIRSIZ);
  iput(ip);
  iput(dp);
  end_op();

  // Only regular files are kept; removing a directory is not undone.
  if (type != T_FILE)
    return 0;
  if (undo_record(&r) < 0)
    return 0;
  myname(name);
  return undo_dir(name, 0);
}

// Called from sys_unlink(), inside its transaction and with the
// parent directory dp locked, just before name is removed from dp.
// Gives the file a second name inside the undo directory udp. Its
// link count then cannot reach zero, so its contents are not freed.
// Because this happens in the same transaction as the removal,
// there is never a moment on disk when the file has no name.
void
undo_keep(struct inode *udp, struct inode *dp, char *name)
{
  char ent[DIRSIZ];
  struct inode *ip;

  if (udp == dp)
    return; // something inside the undo directory itself
  if ((ip = dirlookup(dp, name, 0)) == 0)
    return;
  ilock(ip);
  if (ip->type == T_FILE) {
    entname(ent, 'd', ip->inum);
    ilock(udp);
    if (dirlink(udp, ent, ip->inum) == 0) {
      ip->nlink++;
      iupdate(ip);
    }
    iunlock(udp);
  }
  iunlockput(ip);
}

//
// Rolling back.
//

// Undo U_MODIFY: put the saved contents back.
static void
undo_modify(char *name, struct urec *r)
{
  char ent[DIRSIZ];
  struct inode *ip, *bp;

  entname(ent, 'b', r->inum);
  if ((bp = undo_open(name, ent, 0)) == 0)
    return;
  begin_op();
  ip = iget_type(ROOTDEV, r->inum, T_FILE);
  if (ip) {
    ilock(ip);
    itrunc(ip);
    iunlock(ip);
  }
  end_op();
  if (ip) {
    undo_copy(ip, bp);
    slot_close(ip);
  }
  slot_close(bp);
}

// Undo U_CREATE: remove the name that was created.
static void
undo_create(struct urec *r)
{
  struct inode *dp;

  begin_op();
  if ((dp = iget_type(ROOTDEV, r->pinum, T_DIR)) != 0) {
    ilock(dp);
    dirunlink(dp, r->name);
    iunlockput(dp);
  }
  end_op();
}

// Undo U_DELETE: give the file its name back, then drop the extra
// link that kept it alive.
static void
undo_delete(char *name, struct urec *r)
{
  char ent[DIRSIZ];
  struct inode *ip, *dp, *x;

  begin_op();
  ip = iget_type(ROOTDEV, r->inum, T_FILE);
  dp = iget_type(ROOTDEV, r->pinum, T_DIR);
  if (ip && dp) {
    ilock(dp);
    if ((x = dirlookup(dp, r->name, 0)) != 0) {
      iput(x); // the name is there: already undone, or never removed
    } else if (dirlink(dp, r->name, ip->inum) == 0) {
      ilock(ip);
      ip->nlink++;
      iupdate(ip);
      iunlock(ip);
    }
    iunlock(dp);
  }
  if (ip)
    iput(ip);
  if (dp)
    iput(dp);
  end_op();

  entname(ent, 'd', r->inum);
  undo_remove(name, ent);
}

// Take back every change in the session's journal, newest first.
// Does not empty the journal: undo_discard() does that, afterwards.
// Caller holds undolock.
void
undo_rollback(char *name)
{
  struct inode *jp;
  struct ujhdr hdr;
  struct urec r;
  uint size;
  int i, n;

  if ((jp = undo_open(name, "journal", 0)) == 0)
    return;
  ilock(jp);
  size = jp->size;
  iunlock(jp);
  if (size < sizeof(hdr) || slot_read(jp, 0, (char *)&hdr, sizeof(hdr)) < 0 ||
      hdr.magic != UNDO_MAGIC) {
    slot_close(jp);
    return;
  }

  // Newest first: if a file was deleted and then a new one created
  // under the same name, the new one has to go before the old one
  // can have its name back.
  n = (size - sizeof(hdr)) / sizeof(r);
  for (i = n - 1; i >= 0; i--) {
    if (slot_read(jp, sizeof(hdr) + i * sizeof(r), (char *)&r, sizeof(r)) < 0)
      break;
    switch (r.op) {
    case U_MODIFY:
      undo_modify(name, &r);
      break;
    case U_CREATE:
      undo_create(&r);
      break;
    case U_DELETE:
      undo_delete(name, &r);
      break;
    }
  }
  slot_close(jp);
}

// Throw the session's journal away: its changes can no longer be
// undone. Files that were deleted are really freed now.
// Caller holds undolock.
void
undo_discard(char *name)
{
  char path[UPATH], ent[DIRSIZ];
  struct inode *jp, *udp, *dp;
  struct dirent de;
  uint off;

  // 1. Empty the journal, in one transaction. From here on there is
  //    nothing to roll back, so what follows may be interrupted.
  if ((jp = undo_open(name, "journal", 0)) != 0) {
    begin_op();
    ilock(jp);
    itrunc(jp);
    iunlock(jp);
    end_op();
    slot_close(jp);
  }

  if ((udp = undo_dir(name, 0)) == 0)
    return;

  // 2. Remove everything in the undo directory. Dropping the extra
  //    link of a deleted file is what finally frees it.
  for (off = 2 * sizeof(de);; off += sizeof(de)) {
    begin_op();
    ilock(udp);
    if (readi(udp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
      iunlock(udp);
      end_op();
      break;
    }
    if (de.inum != 0)
      dirunlink(udp, de.name);
    iunlock(udp);
    end_op();
  }

  // 3. Remove the directory itself.
  upath(path, name, 0);
  begin_op();
  if ((dp = nameiparent(path, ent)) != 0) {
    ilock(dp);
    dirunlink(dp, ent);
    iunlockput(dp);
  }
  iput(udp);
  end_op();
}

//
// The undo() system call.
//

// Start journaling the calling process's file changes, and those of
// the children it forks from now on.
int
undo_begin(void)
{
  struct proc *p = myproc();
  char name[SNAMELEN];

  acquire(&p->lock);
  if (p->undo_on || p->as_interval > 0) {
    release(&p->lock);
    return -1;
  }
  release(&p->lock);

  // The session is named after the process: "tx<pid>".
  entname(name + 1, 'x', p->pid % 1000000);
  name[0] = 't';

  // A journal left under this name by an earlier process is stale.
  acquiresleep(&undolock);
  undo_discard(name);
  releasesleep(&undolock);

  acquire(&p->lock);
  safestrcpy(p->sname, name, sizeof(p->sname));
  p->undo_on = 1;
  release(&p->lock);
  return 0;
}

// Stop journaling. If revert is set, take every journaled change
// back; otherwise keep them. Either way the journal is discarded.
int
undo_end(int revert)
{
  struct proc *p = myproc();
  char name[SNAMELEN];

  acquire(&p->lock);
  if (!p->undo_on) {
    release(&p->lock);
    return -1;
  }
  p->undo_on = 0;
  safestrcpy(name, p->sname, sizeof(name));
  release(&p->lock);

  acquiresleep(&undolock);
  if (revert)
    undo_rollback(name);
  undo_discard(name);
  releasesleep(&undolock);
  return 0;
}

// Called once at boot. A "tx" journal that is still on disk belongs
// to a command that was running under undo when the machine went
// down. It did not finish, so its changes are taken back.
void
undo_recover(void)
{
  char name[DIRSIZ + 1];
  struct inode *root;
  struct dirent de;
  uint off;
  int n;

  begin_op();
  root = namei("/");
  end_op();
  if (root == 0)
    return;

  acquiresleep(&undolock);
  for (off = 0;; off += sizeof(de)) {
    ilock(root);
    n = readi(root, 0, (uint64)&de, off, sizeof(de));
    iunlock(root);
    if (n != sizeof(de))
      break;
    if (de.inum == 0)
      continue;
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    n = strlen(name);
    // "tx<digits>.u", and short enough to be a session name
    if (n < 5 || n - 2 >= SNAMELEN || name[0] != 't' || name[1] != 'x' ||
        name[n - 2] != '.' || name[n - 1] != 'u')
      continue;
    name[n - 2] = 0;
    printk("undo: %s did not finish; taking its changes back\n", name);
    undo_rollback(name);
    undo_discard(name);
  }
  releasesleep(&undolock);
  slot_close(root);
}

// snapinfo NAME: describe the two snapshot slots of NAME.
// Snapshots are ordinary files, so this needs no system call of its
// own: it reads each slot's header.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/snap.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct snaphdr h;
  char path[32];
  uint best = 0;
  int slot, fd, n, newest = -1;

  if (argc != 2 || strlen(argv[1]) >= SNAMELEN) {
    fprintf(2, "usage: snapinfo name\n");
    exit(1);
  }

  for (slot = 0; slot < 2; slot++) {
    // "/<name>.s<slot>"
    n = strlen(argv[1]);
    path[0] = '/';
    memmove(path + 1, argv[1], n);
    path[n + 1] = '.';
    path[n + 2] = 's';
    path[n + 3] = '0' + slot;
    path[n + 4] = 0;

    if ((fd = open(path, O_RDONLY)) < 0) {
      printf("%s: none\n", path + 1);
      continue;
    }
    n = read(fd, &h, sizeof(h));
    close(fd);
    if (n != sizeof(h) || h.magic != SNAP_MAGIC) {
      printf("%s: not a snapshot\n", path + 1);
      continue;
    }
    if (h.valid != 1) {
      printf("%s: incomplete (a save was interrupted)\n", path + 1);
      continue;
    }
    printf("%s: generation %d, program %s, %d pages, saved at tick %d", path + 1,
           h.generation, h.name, h.npages, h.saved_at);
    if (h.interval > 0)
      printf(", every %d ticks", h.interval);
    printf("\n");
    if (h.generation > best) {
      best = h.generation;
      newest = slot;
    }
  }

  if (newest < 0) {
    printf("no usable snapshot\n");
    exit(1);
  }
  printf("restore would use %s.s%d\n", argv[1], newest);
  exit(0);
}

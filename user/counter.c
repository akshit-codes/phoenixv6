// counter [-k] FILE N: append N numbered lines to FILE, slowly.
//
// A test program for snapshots. Each line holds the line number and
// a running checksum. The checksum lives only in memory, so if the
// program is restored wrongly the file shows it: a line is missing,
// repeated, or carries the wrong sum.
//
// By default each line is added the way xv6 programs append: open
// the file, read to its end, write, close. With -k the file is
// opened once and kept open, which exercises saving and restoring
// an open file and its offset.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// Write "i sum\n" into buf; return its length.
static int
fmt(char *buf, uint i, uint sum)
{
  char tmp[12];
  uint v[2] = {i, sum};
  int n = 0;

  for (int k = 0; k < 2; k++) {
    int t = 0;
    uint x = v[k];
    do {
      tmp[t++] = '0' + x % 10;
      x /= 10;
    } while (x);
    while (t > 0)
      buf[n++] = tmp[--t];
    buf[n++] = k == 0 ? ' ' : '\n';
  }
  return n;
}

// Open path with the offset at its end.
static int
open_at_end(char *path)
{
  char skip[64];
  int fd = open(path, O_RDWR);

  if (fd < 0)
    return -1;
  while (read(fd, skip, sizeof(skip)) > 0)
    ;
  return fd;
}

int
main(int argc, char *argv[])
{
  int keep = 0, fd = -1, n, len;
  uint i, sum = 0;
  char line[32];

  if (argc > 1 && strcmp(argv[1], "-k") == 0) {
    keep = 1;
    argv++;
    argc--;
  }
  if (argc != 3) {
    fprintf(2, "usage: counter [-k] file n\n");
    exit(1);
  }
  n = atoi(argv[2]);

  // Start with an empty file.
  fd = open(argv[1], O_CREATE | O_WRONLY | O_TRUNC);
  if (fd < 0) {
    fprintf(2, "counter: cannot create %s\n", argv[1]);
    exit(1);
  }
  if (!keep)
    close(fd);

  printf("counter: pid %d\n", getpid());

  for (i = 1; i <= n; i++) {
    sum = sum * 31 + i;
    len = fmt(line, i, sum);
    if (!keep && (fd = open_at_end(argv[1])) < 0) {
      fprintf(2, "counter: cannot open %s\n", argv[1]);
      exit(1);
    }
    if (write(fd, line, len) != len) {
      fprintf(2, "counter: write failed at line %d\n", i);
      exit(1);
    }
    if (!keep)
      close(fd);
    pause(2);
  }

  printf("counter: done, %d lines, sum %u\n", n, sum);
  exit(0);
}

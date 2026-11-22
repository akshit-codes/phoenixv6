// restore NAME: bring the process saved under NAME back to life and
// wait for it, as the shell would for a command it started.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid, status;

  if (argc != 2) {
    fprintf(2, "usage: restore name\n");
    exit(1);
  }
  if ((pid = restore(argv[1])) < 0) {
    fprintf(2, "restore: no usable snapshot called %s\n", argv[1]);
    exit(1);
  }
  printf("restore: %s is running again as pid %d\n", argv[1], pid);

  // The restored process is our child.
  status = 1;
  wait(&status);
  exit(status);
}

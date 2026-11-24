// restore [-n] NAME: bring the process saved under NAME back to
// life and wait for it, as the shell would for a command it started.
// The files it had changed since the snapshot are put back first.
// With -n they are not, which shows why they need to be.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid, status, norollback = 0;

  if (argc > 1 && strcmp(argv[1], "-n") == 0) {
    norollback = 1;
    argv++;
    argc--;
  }
  if (argc != 2) {
    fprintf(2, "usage: restore [-n] name\n");
    exit(1);
  }
  if ((pid = restore(argv[1], norollback)) < 0) {
    fprintf(2, "restore: no usable snapshot called %s\n", argv[1]);
    exit(1);
  }
  printf("restore: %s is running again as pid %d%s\n", argv[1], pid,
         norollback ? " (files not rolled back)" : "");

  // The restored process is our child.
  status = 1;
  wait(&status);
  exit(status);
}

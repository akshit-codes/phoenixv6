// checkpoint PID NAME: ask process PID to save a snapshot of itself
// under NAME. The process does the saving, a moment later; the
// kernel prints a line when it is done.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 3) {
    fprintf(2, "usage: checkpoint pid name\n");
    exit(1);
  }
  if (checkpoint(atoi(argv[1]), argv[2]) < 0) {
    fprintf(2, "checkpoint: no such process, or bad name (1-8 letters or digits)\n");
    exit(1);
  }
  exit(0);
}

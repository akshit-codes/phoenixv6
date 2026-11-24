// crashat N: make the kernel stop dead the next time it reaches
// crash point N (listed in kernel/snap.c). For testing that a power
// cut at the worst moments is survived. "crashat 0" turns it off.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(2, "usage: crashat n\n");
    exit(1);
  }
  crashat(atoi(argv[1]));
  exit(0);
}

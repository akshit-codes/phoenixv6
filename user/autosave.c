// autosave PID TICKS NAME: have process PID save a snapshot of
// itself under NAME every TICKS timer ticks (10 ticks is about one
// second). "autosave PID 0" turns it off.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid, ticks;

  if (argc < 3 || argc > 4) {
    fprintf(2, "usage: autosave pid ticks name\n       autosave pid 0\n");
    exit(1);
  }
  pid = atoi(argv[1]);
  ticks = atoi(argv[2]);
  if (ticks > 0 && argc != 4) {
    fprintf(2, "autosave: a name is needed\n");
    exit(1);
  }
  if (autosave(pid, ticks, argc == 4 ? argv[3] : "") < 0) {
    fprintf(2, "autosave: no such process, or bad name (1-8 letters or digits)\n");
    exit(1);
  }
  if (ticks > 0)
    printf("autosave: pid %d is saved as %s every %d ticks\n", pid, argv[3], ticks);
  else
    printf("autosave: off for pid %d\n", pid);
  exit(0);
}

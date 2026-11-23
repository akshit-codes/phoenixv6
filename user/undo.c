// undo [-n] COMMAND [ARGS...]: run COMMAND with its file changes
// journaled. If it exits with status 0 the changes are kept; if it
// fails or is killed, every file it overwrote, truncated or deleted
// is put back and every file it created is removed.
// With -n the changes are always taken back (a dry run).

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/undo.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int dry = 0, pid, status, keep;

  if (argc > 1 && strcmp(argv[1], "-n") == 0) {
    dry = 1;
    argv++;
    argc--;
  }
  if (argc < 2) {
    fprintf(2, "usage: undo [-n] command [args...]\n");
    exit(1);
  }

  // From here on, what this process and its children do to files
  // is journaled by the kernel.
  if (undo(UNDO_BEGIN) < 0) {
    fprintf(2, "undo: cannot start journaling\n");
    exit(1);
  }

  if ((pid = fork()) < 0) {
    fprintf(2, "undo: fork failed\n");
    undo(UNDO_ABORT);
    exit(1);
  }
  if (pid == 0) {
    exec(argv[1], argv + 1);
    fprintf(2, "undo: cannot run %s\n", argv[1]);
    exit(1);
  }

  status = 1;
  wait(&status);

  keep = status == 0 && !dry;
  if (undo(keep ? UNDO_COMMIT : UNDO_ABORT) < 0) {
    fprintf(2, "undo: could not finish\n");
    exit(1);
  }
  printf("undo: %s exited with status %d; its file changes were %s\n", argv[1],
         status, keep ? "kept" : "taken back");
  exit(status);
}

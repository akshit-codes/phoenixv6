// The undo journal (see undo.c).

#define UNDO_MAGIC 0x554e444f // "UNDO"
#define UPATH      32         // room for "/<name>.u/<entry>"

// Operations of the undo() system call.
#define UNDO_BEGIN  1 // start journaling this process and its children
#define UNDO_COMMIT 2 // stop, keeping the changes
#define UNDO_ABORT  3 // stop, taking the changes back

// Kinds of journal record.
enum { U_MODIFY = 1, U_CREATE, U_DELETE };

// The journal file starts with this.
struct ujhdr {
  uint magic;
  uint epoch; // generation of the snapshot the records follow, or 0
};

// One journaled change. Which fields are used depends on op.
struct urec {
  uint op;
  uint inum;         // the file changed              (U_MODIFY, U_DELETE)
  uint pinum;        // the directory it is named in  (U_CREATE, U_DELETE)
  uint oldsize;      // its size before the change    (U_MODIFY)
  char name[DIRSIZ]; // its name in that directory    (U_CREATE, U_DELETE)
};

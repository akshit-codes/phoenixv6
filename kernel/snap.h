// On-disk format of a process snapshot (see snap.c).
//
// A snapshot is an ordinary file:
//
//   block 0            struct snaphdr, padded to BSIZE
//   byte SNAP_DATA...  npages records, each a struct snappage
//                      followed by the PGSIZE bytes of that page
//
// The header is written twice: first with valid == 0, before any
// page, and again with valid == 1 after the last page. A snapshot
// is only ever used if valid == 1.

#define SNAP_MAGIC 0x534e4150 // "SNAP"
#define SNAP_DATA  BSIZE      // file offset of the first page record

// One open file descriptor.
struct snapfile {
  short type;    // FD_NONE, FD_INODE or FD_DEVICE
  short major;   // FD_DEVICE
  short dupof;   // lower fd that shares the same struct file, or -1
  char readable;
  char writable;
  uint inum; // FD_INODE
  uint off;  // FD_INODE
};

struct snaphdr {
  uint magic;
  uint valid;    // 1 once every page has been written
  uint npages;   // page records that follow
  uint cwd_inum; // current directory
  uint64 sz;     // p->sz
  char name[16]; // p->name
  struct trapframe tf; // all user registers and the user pc
  struct snapfile files[NOFILE];
};

// Header of one saved page; the page's contents follow it.
struct snappage {
  uint64 va;    // page-aligned user virtual address
  uint64 flags; // PTE_FLAGS() of the page's PTE
};

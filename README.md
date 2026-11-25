# phoenixv6

**xv6 that rises again after a crash.** AutoSave and Undo for [xv6-riscv](https://github.com/mit-pdos/xv6-riscv), MIT's teaching operating system.

A phoenix burns and rises from its own ashes. Under phoenixv6 a running program can lose power in the middle of its work and continue after the reboot from its last snapshot, with its files put back to match.

This is a course project. It adds two things to the xv6 kernel, built on one mechanism.

**AutoSave.** The kernel periodically saves a complete snapshot of a chosen running program to disk. The program is not modified and does not know. If it is killed, or the machine loses power, `restore` brings it back and it continues from the last snapshot, including after a reboot.

**Undo.** `undo command` runs a command and journals every file change it makes. If the command fails, every file it overwrote, truncated or deleted is put back, and every file it created is removed. The idea comes from Ethereum, where a failed transaction is reverted as if it never ran.

**Why they are one project.** Restoring a program's memory without its files is wrong. Take a program that appends a line to a file on each step:

| | In memory | In the file |
| --- | --- | --- |
| Snapshot | step 13 | 13 lines |
| Power cut | step 21 | 21 lines |
| Restore memory only | step 13 | 21 lines |
| Program finishes | step 50 | 58 lines, 14 to 21 twice |

So from a process's first snapshot on, the kernel journals its file changes, and `restore` rolls the files back to the moment of the snapshot before the program resumes. The claim the tests check:

> After a power cut at any moment, a restored program finishes with output byte-for-byte identical to a run that was never interrupted.

## Building and running

You need a RISC-V toolchain (`riscv64-unknown-elf-gcc`, `riscv64-elf-gcc` or `riscv64-linux-gnu-gcc`) and `qemu-system-riscv64`, the same as for stock xv6.

```
make qemu          build everything and boot; Ctrl-a x quits qemu
```

The disk image `fs.img` is kept between boots, which is what lets a snapshot outlive a power cut. `make clean` removes it.

## Commands

| Command | What it does |
| --- | --- |
| `autosave PID TICKS NAME` | Save process PID every TICKS timer ticks (10 is about a second) under NAME. `autosave PID 0` stops |
| `checkpoint PID NAME` | Save process PID once, now |
| `restore NAME` | Roll files back, bring the newest usable snapshot to life, wait for it. `-n` skips the rollback, to show what goes wrong |
| `snapinfo NAME` | Describe the two snapshot slots of NAME |
| `undo COMMAND...` | Run COMMAND; keep its file changes if it exits 0, take them back otherwise. `-n` always takes them back |
| `counter [-k] FILE N` | Test program: appends N lines with a running checksum that lives only in memory |
| `badprog [setup\|ok]` | Test program: damages some files, then fails |
| `crashat N` | Testing: stop the kernel at a chosen point in the middle of a disk update |

## A two-minute demonstration

```
$ make qemu
$ counter out 50 &
counter: pid 4
$ autosave 4 5 job
$ snapinfo job
      ... press Ctrl-a x: qemu exits, which is a power cut ...
$ make qemu
$ restore job
restore: job is running again as pid 3
counter: done, 50 lines, sum 3083686713
$ cat out                    50 lines, none repeated
```

```
$ badprog setup
$ cat a.txt b.txt c.txt
$ undo badprog               overwrites a.txt, empties b.txt, deletes c.txt, creates d.txt
undo: badprog exited with status 1; its file changes were taken back
$ cat a.txt b.txt c.txt      as before
```

## How it works

**Taking a snapshot.** A process saves itself, at the end of `usertrap()`, just before returning to user space. There its trapframe holds its complete user CPU state and its memory cannot change, so the two describe one instant. Another process only leaves a request (`autosave`, `checkpoint`), the way `kill` only sets a flag. The save walks the page table as `uvmcopy()` does and writes each mapped page, with its address and permission bits, to a file. Open files are recorded by inode number and offset.

**Restoring.** `restore` is `fork()` with a file in place of the parent: `allocproc()`, then pages, trapframe, open files and current directory are read from the snapshot. The new process enters user space through `forkret()` like any forked child and resumes at the saved program counter.

**Surviving a power cut during a save.** A snapshot is far larger than one xv6 log transaction (10 blocks), so the log cannot make a save atomic. Each name has two snapshot files. A save writes into the one that does not hold the newest complete snapshot, and a final one-block header write, which is a single transaction, makes it count. At every moment at least one complete snapshot exists.

**The undo journal.** Hooks in `filewrite()`, `sys_open()`, `sys_mkdir()` and `sys_unlink()` record a change before it is made: the first write to a file saves a copy of it; a create records the name; a delete gives the file a hidden second link in the same transaction, so its contents are never freed. Rolling back replays the records newest first. Every step can be repeated and the journal is emptied last, so a crash during a rollback is cured by running it again.

**Joining them.** Each snapshot has a generation number, and the journal header carries the generation of the snapshot it follows. Committing a snapshot resets the journal. `restore` rolls the journal back only if the two numbers match; if the power failed between committing a snapshot and resetting the journal, they differ, and the journal (which describes changes the snapshot already includes) is discarded instead.

## Source

| File | Contents |
| --- | --- |
| `kernel/snap.c`, `kernel/snap.h` | Snapshot format, saving, the two slots, restoring |
| `kernel/undo.c`, `kernel/undo.h` | The journal, the hooks, rollback, recovery at boot |
| `kernel/trap.c` | The check in `usertrap()` that triggers a save |
| `kernel/proc.c`, `kernel/proc.h` | Per-process state; `snap_request()`, `snap_autosave()` |
| `kernel/file.c`, `kernel/sysfile.c` | The journal hooks; `dirunlink()` split out of `sys_unlink()` |
| `kernel/fs.c` | `iget_type()`: an inode by number, only if it still exists |
| `kernel/sysproc.c` | The five system calls: `checkpoint`, `restore`, `autosave`, `undo`, `crashat` |
| `user/` | `autosave`, `checkpoint`, `restore`, `snapinfo`, `undo`, `counter`, `badprog`, `crashat` |
| `test-autosave.py` | The tests below |

## Tests

```
./test-autosave.py                  everything
./test-autosave.py -n 50 power_cut_random
./test-xv6.py -q usertests          the stock xv6 tests still pass
```

| Test | What it establishes |
| --- | --- |
| `checkpoint_same_boot` | Memory, registers, open files and offsets are restored correctly |
| `power_cut_random` | Autosave on, power cut at a random moment, reboot, restore: output identical |
| `power_cut_twice` | A restored process is itself autosaved |
| `crash_1_...` | Power lost after the last page of a save, before the header: the older slot is used |
| `crash_2_...` | Power lost after a snapshot commits, before its journal is reset: no rollback, correctly |
| `crash_3_...` | Power lost in the middle of a rollback, with a file emptied: restoring again repairs it |
| `crash_4_...` | Power lost half-way through resetting the journal |
| `without_rollback_the_file_is_wrong` | With rollback switched off the output has repeated lines: the journal is necessary |
| `undo_...` | Failed command reverted; successful command kept; dry run; power cut during a command reverted at boot |
| `refusals` | A process with a pipe, a bad name, a missing snapshot, a name still in use: an error message, never a panic |

## Limits

- One process per name. Children of an autosaved program have their file changes rolled back, but their memory is not saved.
- A process with a pipe open cannot be saved: a pipe's contents live in kernel memory shared with another process.
- About 65 pages (260 KB) of memory per snapshot, which is xv6's maximum file size.
- A restored process has a new PID.
- Console output cannot be taken back; a restored program prints its last few lines again.
- The first write to a file after each snapshot copies the whole file.
- The program is paused while it is being saved.
- Not journaled: `link`, `mknod`, removing a directory, and changes made by processes outside the session.

## Credits and license

phoenixv6 is built on xv6-riscv by Frans Kaashoek, Robert Morris, Russ Cox and the MIT PDOS group. Their original README is kept unchanged as [`README`](README) (the build also copies it into the xv6 disk image, and the stock tests read it). Everything is under the MIT license in [`LICENSE`](LICENSE).

#!/usr/bin/env python3

#
# Tests for AutoSave and Undo. Boots xv6 in qemu, types commands at
# its shell, cuts the power (kills qemu) at random or at chosen
# points, boots again on the same disk, and checks the files.
#
#   ./test-autosave.py            run every test
#   ./test-autosave.py crash      run the tests whose name contains "crash"
#   ./test-autosave.py -n 50 power_cut    50 random power cuts
#
# The measure of success throughout: the file written by the test
# program "counter" must be byte-for-byte what an uninterrupted run
# would have written.
#

import argparse, os, random, re, signal, subprocess, sys, time

parser = argparse.ArgumentParser()
parser.add_argument('pattern', nargs='?', default='', help='run tests whose name contains this')
parser.add_argument('-n', type=int, default=10, help='number of random power cuts (default 10)')
parser.add_argument('-s', type=int, default=1, help='random seed')
args = parser.parse_args()

QEMU = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
        '-kernel', 'kernel/kernel', '-m', '128M', '-smp', '3', '-nographic',
        '-global', 'virtio-mmio.force-legacy=false',
        '-drive', 'file=fs.img,if=none,format=raw,id=x0',
        '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0']

LINES = 50  # how many lines counter writes in most tests


class Failed(Exception):
    pass


class Xv6:
    """One boot of xv6."""

    def __init__(self, fresh=False):
        if fresh:  # start from a newly made disk
            subprocess.run(['rm', '-f', 'fs.img'], check=True)
            subprocess.run(['make', '-s', 'fs.img'], check=True, stdout=subprocess.DEVNULL)
        self.proc = subprocess.Popen(QEMU, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.out = ''
        self.mark = 0
        self.wait(r'\$ $', 30)

    def read(self):
        try:
            b = os.read(self.proc.stdout.fileno(), 65536)
            if b:
                self.out += b.decode('utf-8', 'replace').replace('\r', '')
        except BlockingIOError:
            pass

    def type(self, line):
        """type a command; do not wait for it"""
        self.read()
        self.mark = len(self.out)
        self.proc.stdin.write((line + '\n').encode())
        self.proc.stdin.flush()

    def wait(self, regexp, timeout=40):
        """wait for output matching regexp since the last command"""
        end = time.time() + timeout
        while time.time() < end:
            self.read()
            m = re.search(regexp, self.out[self.mark:], re.M)
            if m:
                return m
            time.sleep(0.05)
        tail = self.out[-600:]
        self.power_cut()
        raise Failed('timed out waiting for %r; last output:\n%s' % (regexp, tail))

    def run(self, line, timeout=40):
        """run a command to completion and return what it printed"""
        self.type(line)
        self.wait(r'\$ $', timeout)
        return self.out[self.mark:]

    def power_cut(self):
        """kill qemu: whatever has not reached the disk is lost"""
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()

    def lines_of(self, path):
        """the "number number" lines of a file written by counter"""
        return [l for l in self.run('cat ' + path).split('\n') if re.match(r'^\d+ \d+$', l)]

    def start_counter(self, flags='', n=LINES):
        """start counter in the background; return its pid"""
        self.type('counter %sout %d &' % (flags, n))
        return self.wait(r'counter: pid (\d+)').group(1)

    def restore(self, flags='', n=LINES):
        self.type('restore %sjob' % flags)
        self.wait(r'restore: job is running again')
        self.wait(r'counter: done, %d lines' % n, 90)
        self.wait(r'\$ $')


def expected(n=LINES):
    """what counter writes if nothing interrupts it"""
    s, lines = 0, []
    for i in range(1, n + 1):
        s = (s * 31 + i) & 0xffffffff
        lines.append('%d %d' % (i, s))
    return lines


def check(cond, why):
    if not cond:
        raise Failed(why)


def check_file(x, n=LINES):
    got = x.lines_of('out')
    if got != expected(n):
        raise Failed('out has %d lines, %d of them repeated; expected %d lines'
                     % (len(got), len(got) - len(set(got)), n))


#
# AutoSave
#

def test_checkpoint_same_boot():
    """save on request, kill the process, restore it in the same boot"""
    for flags in ['', '-k ']:
        x = Xv6(fresh=True)
        pid = x.start_counter(flags)
        time.sleep(2)
        x.type('checkpoint %s job' % pid)
        x.wait(r'snapshot job: saved pid')
        time.sleep(1.5)
        x.run('kill ' + pid)
        time.sleep(0.6)
        x.restore()
        check_file(x)
        x.power_cut()


def test_power_cut_random():
    """autosave on, cut the power at a random moment, reboot, restore"""
    for i in range(args.n):
        flags = '-k ' if i % 3 == 2 else ''
        x = Xv6(fresh=True)
        pid = x.start_counter(flags)
        x.run('autosave %s 5 job' % pid)
        delay = random.uniform(1.0, 8.0)
        time.sleep(delay)
        x.power_cut()
        x = Xv6()
        had = len(x.lines_of('out'))
        x.restore()
        check_file(x)
        print('    cut %2d: after %.1fs, file had %2d lines, finished identical' % (i + 1, delay, had))
        x.power_cut()


def test_power_cut_twice():
    """a restored process is autosaved again, so a second cut is survived too"""
    x = Xv6(fresh=True)
    pid = x.start_counter()
    x.run('autosave %s 5 job' % pid)
    time.sleep(2.5)
    x.power_cut()
    x = Xv6()
    x.type('restore job &')
    x.wait(r'restore: job is running again')
    time.sleep(2.5)
    x.power_cut()
    x = Xv6()
    x.restore()
    check_file(x)
    x.power_cut()


def crash_during_save(point):
    x = Xv6(fresh=True)
    pid = x.start_counter()
    x.run('autosave %s 5 job' % pid)
    time.sleep(2.2)
    x.type('crashat %d' % point)
    x.wait(r'panic: crash point', 20)
    x.power_cut()
    x = Xv6()
    info = x.run('snapinfo job')
    x.restore()
    check_file(x)
    x.power_cut()
    return info


def test_crash_1_pages_written_header_not():
    """stop after the last page of a save, before the header that commits it"""
    info = crash_during_save(1)
    check('incomplete' in info, 'expected one slot to be incomplete:\n' + info)


def test_crash_2_snapshot_committed_journal_not_reset():
    """stop after a snapshot commits, before its journal is reset: restore
    must NOT roll back, because the journal is older than the snapshot"""
    info = crash_during_save(2)
    check('incomplete' not in info, 'expected both slots complete:\n' + info)


def test_crash_4_journal_emptied_backups_not_removed():
    """stop half-way through resetting the journal"""
    crash_during_save(4)


def test_crash_3_during_rollback():
    """stop in the middle of a rollback, with the file emptied and not yet
    refilled; restoring again must repeat the rollback"""
    x = Xv6(fresh=True)
    pid = x.start_counter()
    x.run('autosave %s 20 job' % pid)   # save rarely, so there is something to roll back
    time.sleep(3.5)
    x.power_cut()
    x = Xv6()
    x.run('crashat 3')
    x.type('restore job')
    x.wait(r'panic: crash point', 20)
    x.power_cut()
    x = Xv6()
    check(len(x.lines_of('out')) == 0, 'expected out to be empty after the interrupted rollback')
    x.restore()
    check_file(x)
    x.power_cut()


def test_without_rollback_the_file_is_wrong():
    """the problem the journal solves: restore memory only, and lines repeat"""
    x = Xv6(fresh=True)
    pid = x.start_counter()
    x.run('autosave %s 20 job' % pid)   # save rarely, so plenty is written after a save
    time.sleep(3.5)
    x.power_cut()
    x = Xv6()
    x.restore('-n ')
    got = x.lines_of('out')
    repeated = len(got) - len(set(got))
    print('    with rollback switched off: %d lines, %d repeated' % (len(got), repeated))
    check(repeated > 0, 'expected repeated lines when files are not rolled back')
    x.power_cut()


def test_refusals():
    """things that cannot be done fail with a message, not a panic"""
    x = Xv6(fresh=True)
    out = x.run('restore nosuch')
    check('no usable snapshot' in out, 'restore of a missing name: ' + out)
    out = x.run('checkpoint 999 job')
    check('no such process' in out, 'checkpoint of a missing pid: ' + out)
    out = x.run('checkpoint 1 a/b')
    check('bad name' in out, 'checkpoint with a bad name: ' + out)
    # a process with a pipe open
    x.type('counter -k out 30 | cat &')
    pid = x.wait(r'counter: pid (\d+)').group(1)
    x.type('checkpoint %s job' % pid)
    x.wait(r'has a pipe open; not saved')
    # restoring while the original still runs
    pid = x.start_counter()
    x.run('autosave %s 5 job' % pid)
    time.sleep(1.5)
    x.type('restore job')
    x.wait(r'still running')
    check('panic' not in x.out, 'kernel panicked')
    x.power_cut()


#
# Undo
#

FILES = ['a.txt', 'b.txt', 'c.txt', 'd.txt', 'newdir/e.txt']


def state(x):
    s = {f: x.run('cat ' + f).split('\n', 1)[1].rsplit('$', 1)[0] for f in FILES}
    s['names'] = sorted(l.split()[0] for l in x.run('ls').split('\n')[1:]
                        if l.strip() and not l.startswith('$'))
    return s


def test_undo_reverts_a_failed_command():
    x = Xv6(fresh=True)
    x.run('badprog setup')
    before = state(x)
    out = x.run('undo badprog')
    check('taken back' in out, out)
    check(state(x) == before, 'files differ after undo')
    x.power_cut()


def test_undo_keeps_a_successful_command():
    x = Xv6(fresh=True)
    x.run('badprog setup')
    before = state(x)
    out = x.run('undo badprog ok')
    check('kept' in out, out)
    after = state(x)
    check(after != before and 'new file' in after['d.txt'], 'changes were not kept')
    check(not any('.u' in n for n in after['names']), 'journal directory left behind')
    x.power_cut()


def test_undo_dry_run():
    x = Xv6(fresh=True)
    x.run('badprog setup')
    before = state(x)
    out = x.run('undo -n badprog ok')
    check('taken back' in out, out)
    check(state(x) == before, 'files differ after a dry run')
    x.power_cut()


def test_undo_power_cut_reverts_at_boot():
    """a command under undo that is cut short by a power cut is reverted at boot"""
    x = Xv6(fresh=True)
    x.run('counter keep 3')
    x.type('undo counter keep 40 &')
    x.wait(r'counter: pid')
    time.sleep(2.5)
    x.power_cut()
    x = Xv6()
    check('did not finish' in x.out, 'no recovery message at boot')
    check(x.lines_of('keep') == expected(3), 'keep was not put back')
    x.power_cut()


def main():
    random.seed(args.s)
    subprocess.run(['make', '-s', 'kernel/kernel', 'fs.img'], check=True, stdout=subprocess.DEVNULL)
    tests = [(n, f) for n, f in globals().items() if n.startswith('test_') and args.pattern in n]
    failed = 0
    for name, f in tests:
        print('%s' % name[5:])
        start = time.time()
        try:
            f()
            print('  PASS (%.0fs)' % (time.time() - start))
        except Failed as e:
            failed += 1
            print('  FAIL: %s' % e)
    print('%d of %d tests passed' % (len(tests) - failed, len(tests)))
    sys.exit(1 if failed else 0)


main()

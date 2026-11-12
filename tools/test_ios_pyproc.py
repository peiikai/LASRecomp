#!/usr/bin/env python3
"""runtime/platform/ios_pyproc.py under a desktop Python, set up as the iOS app is: one interpreter, the script on a
thread, no fork / exec (subprocess's own switch for iOS), and a "runtime" thread that looks at the working directory
and opens a file relative to it the whole time. The script below does what a script may do with directories,
threads, the environment and child processes; then a Cancel in the middle of a child. macOS only (the per-thread
working directory is Darwin's)."""
import ctypes
import os
import subprocess
import sys
import tempfile
import threading
import time

if sys.platform != 'darwin' or sys.version_info < (3, 11):
    print('skipped: macOS only, Python 3.11 or later')
    sys.exit(0)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'platform'))
import ios_pyproc  # noqa: E402

SCRIPT = r'''
import concurrent.futures, contextlib, os, subprocess, sys, threading, time, _thread
import state
scratch, py = os.path.realpath(sys.argv[1]), sys.executable
A, B, C = (os.path.join(scratch, n) for n in 'ABC')
for d in (A, B, C):
    os.makedirs(d)
    open(os.path.join(d, 'here.txt'), 'w').write(os.path.basename(d))

def check(what, got, want):
    print('ok' if got == want else f'FAIL: {what}: {got!r}, expected {want!r}', flush=True)

def here():
    try: return open('here.txt').read()
    except OSError as e: return type(e).__name__

def in_thread(fn=here):
    r = []
    t = threading.Thread(target=lambda: r.append(fn()))
    t.start(); t.join()
    return r[0]

def child(code, *args, **kw):
    return subprocess.run([py, '-c', code, *args], capture_output=True, text=True, **kw)

# the working directory
os.chdir(A)
check('chdir', (here(), os.getcwd(), os.path.abspath('x')), ('A', A, os.path.join(A, 'x')))
with contextlib.chdir(B):
    check('contextlib.chdir', here(), 'B')
fd = os.open(C, os.O_RDONLY)
os.fchdir(fd); os.close(fd)
check('fchdir', here(), 'C')
try:
    os.chdir(os.path.join(scratch, 'missing'))
except FileNotFoundError as e:
    check('chdir to a missing folder', e.errno, 2)
else:
    check('chdir to a missing folder', 'no error', 'FileNotFoundError')
os.chdir(A)

# threads start where their starter is
check('threading.Thread', in_thread(), 'A')
with concurrent.futures.ThreadPoolExecutor(4) as ex:
    check('ThreadPoolExecutor', set(ex.map(lambda _: (time.sleep(0.02), here())[1], range(8))), {'A'})
r = []
_thread.start_new_thread(lambda: r.append(here()), ())
time.sleep(0.2)
check('_thread.start_new_thread', r, ['A'])
check('a thread started by a thread', in_thread(in_thread), 'A')
# (not a real process's: a thread already running keeps its folder, and the log says so)
go, seen = threading.Event(), []
t = threading.Thread(target=lambda: (go.wait(), seen.append(here())))
t.start()
os.chdir(B)
go.set(); t.join()
check('a thread already running at a chdir', (seen[0], in_thread()), ('A', 'B'))
os.chdir(A)

# the environment
os.environ['T_BY_SCRIPT'] = '1'
os.putenv('T_BY_PUTENV', '1')
check('os.environ', (os.environ.get('T_BY_SCRIPT'), in_thread(lambda: os.getenv('T_BY_SCRIPT'))), ('1', '1'))

# children
state.n += 1
p = child('import os, sys, state; print(open("here.txt").read(), os.environ.get("T_BY_SCRIPT"), sys.argv, state.n, __name__)', 'x')
check('child: directory, environment, argv, fresh modules', (p.returncode, p.stdout), (0, "A 1 ['-c', 'x'] 0 __main__\n"))
check('  the parent\'s modules after it', (state.n, sys.modules['state'] is state), (1, True))
p = child('import os; print(open("here.txt").read(), os.environ.get("T_CHILD"), os.environ.get("T_BY_SCRIPT"))', cwd=B, env={'T_CHILD': '7'})
check('child with cwd= and env=', p.stdout, 'B 7 None\n')
p = child('import os, sys; os.chdir(sys.argv[1]); os.environ["T_CHILD"] = "1"; sys.path.insert(0, "/x"); sys.argv[0] = "y"; sys.exit(5)', C)
check('what a child changes stays its own', (p.returncode, here(), os.environ.get('T_CHILD'), '/x' in sys.path, sys.argv[0] == __file__),
      (5, 'A', None, False, True))
deep = os.path.join(os.path.dirname(__file__), 'deep.py')
open(deep, 'w').write('import subprocess, sys\nprint(sys.argv[1], flush=True)\nif sys.argv[1] != "0":\n'
                      '    sys.exit(subprocess.call([sys.executable, __file__, str(int(sys.argv[1]) - 1)]) + 1)\n')
p = subprocess.run([py, deep, '3'], capture_output=True, text=True)
check('a child\'s child\'s child', (p.returncode, p.stdout), (3, '3\n2\n1\n0\n'))
p = child('import sys; print("out"); print("err", file=sys.stderr); raise ValueError("boom")')
check('child that raises', (p.returncode, p.stdout, p.stderr.splitlines()[0], p.stderr.splitlines()[-1]), (1, 'out\n', 'err', 'ValueError: boom'))
check('input=', child('import sys; print(sys.stdin.read().upper())', input='typed').stdout, 'TYPED\n')
check('check_output', subprocess.check_output([py, '-c', 'print(1 + 1)']), b'2\n')
try:
    subprocess.check_call([py, '-c', 'raise SystemExit(4)'])
except subprocess.CalledProcessError as e:
    check('check_call', e.returncode, 4)
p = subprocess.Popen([py, '-c', 'print("x" * 200000)'], stdout=subprocess.PIPE)
check('Popen, stdout=PIPE', (len(p.stdout.read()), p.wait()), (200001, 0))
check('-m', subprocess.run([py, '-m', 'json.tool'], input='[1]', capture_output=True, text=True).stdout.split(), ['[', '1', ']'])
with open(os.path.join(scratch, 'out.txt'), 'w') as f:
    subprocess.run([py, '-c', 'print("into the file")'], stdout=f)
check('stdout=<file>', open(os.path.join(scratch, 'out.txt')).read(), 'into the file\n')
print('inherited:', flush=True)
subprocess.run([py, '-c', 'print("child line")'])
check('a child\'s thread', child('import threading\nt = threading.Thread(target=lambda: print(open("here.txt").read()))\nt.start(); t.join()', cwd=C).stdout, 'C\n')
try:
    subprocess.run(['/bin/ls'], capture_output=True)
except OSError as e:
    check('another program', e.errno, 45)
else:
    check('another program', 'ran', 'OSError')
sys.exit(3)
'''

# the parent ignores what its child returned: the Cancel must still end it
KILLED = r'''
import subprocess, sys
print('parent', flush=True)
subprocess.run([sys.executable, '-c', 'import time\nprint("child", flush=True)\nwhile True: time.sleep(0.01)'])
print('FAIL: the parent went on after the Cancel', flush=True)
'''


def check(cond, what):
    if not cond:
        sys.exit(f'FAIL: {what}')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        root, prog = os.path.join(tmp, 'data root'), os.path.join(tmp, 'program')
        os.makedirs(root), os.makedirs(prog)
        for name, text in (('script.py', SCRIPT), ('killed.py', KILLED), ('state.py', 'n = 0\n')):
            with open(os.path.join(prog, name), 'w') as f:
                f.write(text)
        os.chdir(root)
        open('seen.txt', 'w').close()
        subprocess._can_fork_exec = False  # (as on iOS: the real Popen raises ENOTSUP)
        ios_pyproc.install(prog)

        moved, missing, stop = set(), [0], threading.Event()

        def runtime():
            while not stop.is_set():
                moved.add(os.getcwd())
                missing[0] += not os.path.exists('seen.txt')
                time.sleep(0.002)

        def spawn(argv, log):
            r = {}

            def proc():
                r['tid'] = threading.get_ident()
                r['code'] = ios_pyproc.run(argv, log, False)
            t = threading.Thread(target=proc, daemon=True)  # (a failed check must not leave the test waiting for it)
            t.start()
            return t, r

        watch = threading.Thread(target=runtime)
        watch.start()
        try:
            t, r = spawn([os.path.join(prog, 'script.py'), os.path.join(tmp, 'scratch')], 'script.log')
            t.join()
            log = open('script.log').read()
            check(r.get('code') == 3 and 'FAIL' not in log and 'Traceback' not in log, f"the script: exit {r.get('code')}\n{log}")
            check(log.splitlines().count('ok') == 24, f"{log.splitlines().count('ok')} checks passed, expected 24\n{log}")
            check('inherited:\nchild line\n' in log, f'a child\'s inherited output is not in the log\n{log}')
            check(log.count('chdir with other threads running') == 1, f'the note about a running thread at a chdir\n{log}')

            t, r = spawn([os.path.join(prog, 'killed.py')], 'killed.log')
            for _ in range(500):
                if os.path.exists('killed.log') and 'child' in open('killed.log').read():
                    break
                time.sleep(0.01)
            ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(r['tid']), ctypes.py_object(KeyboardInterrupt))  # plat_proc_kill
            t.join(5)
            log = open('killed.log').read()
            check(not t.is_alive() and r.get('code') == 137 and 'FAIL' not in log and log.count('killed') == 2,
                  f"the Cancel: exit {r.get('code')}\n{log}")
        finally:
            stop.set()
            watch.join()
        check(moved == {root} and not missing[0], f'the app\'s working directory moved: {sorted(moved - {root})}, a file under it missing {missing[0]} times')
        check(os.getcwd() == root, 'the working directory after the runs')
        getenv = ctypes.CDLL(None).getenv
        check(not getenv(b'T_BY_SCRIPT') and not getenv(b'T_BY_PUTENV') and 'T_BY_SCRIPT' not in os.environ, 'the script\'s environment reached the app\'s')
        os.chdir(tmp)
    print('ok')


if __name__ == '__main__':
    main()

"""iOS: what makes a script run by plat_spawn (runtime/platform/ios_python.m) a process of its own while it is a
thread of the app's one interpreter - an iOS app can't start a process. The script is not changed for it:

- The working directory. chdir() moves the whole app, and the runtime's files are relative to the data root (the
  log the Randomizer tab reads the progress from, larecomp.ini, the saves). os.chdir here moves the calling thread
  only (pthread_chdir_np: exported by libSystem since iOS 10, not in the SDK's headers), and a thread the script
  starts begins where its starter is. A thread that is already running stays where it was: said in the script's
  log, since a real process would have moved it.
- Child processes. `subprocess` with this Python (sys.executable) and a script, -m or -c runs the child here, on
  the calling thread, to its end: its own argv, directory, environment and output, the program's modules imported
  afresh. The parent waits, as it would in run() / wait() / communicate(). The generator starts itself again this
  way for the next dungeon room layout (tools/rando/las_headless.py retry_layout). Any other program is the
  OSError it was.
- The environment. os.environ is the script's own: its writes and a child's env= never reach setenv(), which the
  app's other threads would race with.

Nothing in it is iOS-only (macOS has the same calls): tools/test_ios_pyproc.py runs it under a desktop Python."""
import ctypes
import errno
import io
import os
import runpy
import subprocess
import sys
import threading
import traceback
import _thread

_libc = ctypes.CDLL(None, use_errno=True)
_libc.pthread_chdir_np.argtypes = [ctypes.c_char_p]
_libc.pthread_fchdir_np.argtypes = [ctypes.c_int]


def _thread_chdir(path):
    """The calling thread's own working directory (a path or a descriptor); -1: the app's again."""
    rc = _libc.pthread_fchdir_np(path) if isinstance(path, int) else _libc.pthread_chdir_np(os.fsencode(path))
    if rc and path != -1:  # (-1 with none of its own: EBADF)
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e), None if isinstance(path, int) else path)


class _Proc:  # one script run as a process; the threads it starts share it
    threads = 1


_tls = threading.local()  # .proc: the process this thread belongs to
_real = {}
_fresh_dir = ''


def _proc():
    return getattr(_tls, 'proc', None)


def _chdir(path):
    p = _proc()
    if not p:
        return _real['chdir'](path)
    _thread_chdir(path)
    if p.threads > 1:
        sys.stderr.write('chdir with other threads running: they stay in the folder they were in\n')


def _inherit(fn):
    p = _proc()
    if not p:
        return fn
    try:
        cwd = os.getcwd()
    except OSError:
        cwd = None

    def run(*a, **k):
        _tls.proc = p
        p.threads += 1
        try:
            if cwd is not None:
                _thread_chdir(cwd)
            return fn(*a, **k)
        finally:
            p.threads -= 1
            _thread_chdir(-1)
    return run


def _starter(real):
    return lambda function, *a, **k: real(_inherit(function), *a, **k)


def _putenv(key, value):
    if not _proc():
        _real['putenv'](key, value)


def _unsetenv(key):
    if not _proc():
        _real['unsetenv'](key)


def _own(name, m, root):
    # (by real path: the bundle is /var/containers/... to NSBundle and /private/var/containers/... to the importer.
    # Namespace packages - RandomizerCore.Tools has no __init__.py - have no __file__, only __path__; left in place
    # one would hand out its old submodules as attributes, with event_tools' AutoEvent counter where the last run
    # left it. The PySide6 stubs the generator puts in go too.)
    paths = [getattr(m, '__file__', None)] + list(getattr(m, '__path__', None) or [])
    return name == 'PySide6' or name.startswith('PySide6.') or any(p and os.path.realpath(p).startswith(root) for p in paths)


def _run_script(argv, out, err, kind='path', env=None, cwd=None, stdin=None):
    """argv as __main__ (kind 'path': argv[0] is a file, 'module': -m, 'code': -c with the source in argv[0]), to
    its end; its exit code. Called from inside a process it is that process's child: what the child changed is the
    parent's again when it returns."""
    parent = _proc()
    saved = (sys.argv, sys.stdout, sys.stderr, sys.stdin, list(sys.path), dict(os.environ._data), os.getcwd())
    # A fresh import of the program's own modules, as a new process would have (the C extensions - oead, quicktex,
    # PIL - stay loaded). Chosen first, removed after: a namespace __path__ recomputes itself through its parent
    # in sys.modules.
    root = os.path.realpath(_fresh_dir) + os.sep
    mine = {name: m for name, m in list(sys.modules.items()) if _own(name, m, root)}
    for name in mine:
        del sys.modules[name]
    if mine and not parent:
        out.write('(%d of the generator\'s modules re-imported)\n' % len(mine))
    _tls.proc = _Proc()
    code, killed = 0, False
    try:
        if env is not None:
            os.environ._data.clear()
            os.environ._data.update({os.environ.encodekey(k): os.environ.encodevalue(v) for k, v in env.items()})
        if cwd is not None:
            _thread_chdir(cwd)
        sys.stdout, sys.stderr = out, err
        if stdin is not None:
            sys.stdin = stdin
        # (sys.path[0] as python sets it: the script's folder, or the working directory)
        sys.path.insert(0, os.path.dirname(os.path.realpath(argv[0])) if kind == 'path' else '')
        if kind == 'code':
            sys.argv = ['-c'] + argv[1:]
            exec(compile(argv[0], '<string>', 'exec'), {'__name__': '__main__'})
        elif kind == 'module':
            sys.argv = list(argv)
            runpy.run_module(argv[0], run_name='__main__', alter_sys=True)
        else:
            sys.argv = list(argv)
            runpy.run_path(argv[0], run_name='__main__')
    except SystemExit as e:
        c = e.code
        if c is None: code = 0
        elif isinstance(c, int): code = c
        else: err.write(str(c) + '\n'); code = 1
    except KeyboardInterrupt:  # (plat_proc_kill)
        err.write('killed\n'); code = 137; killed = True
    except BaseException:
        err.write(traceback.format_exc()); code = 1
    finally:
        for f in (out, err):
            try: f.flush()
            except ValueError: pass  # (the script closed it)
        sys.argv, sys.stdout, sys.stderr, sys.stdin = saved[:4]
        sys.path[:] = saved[4]
        os.environ._data.clear()
        os.environ._data.update(saved[5])
        _tls.proc = parent
        if parent:
            for name in [name for name, m in list(sys.modules.items()) if _own(name, m, root)]:
                del sys.modules[name]
            sys.modules.update(mine)
            _thread_chdir(saved[6])
        else:
            _thread_chdir(-1)
    if killed and parent:
        raise KeyboardInterrupt  # (the Cancel is for the whole tree: the parent stops too)
    return code


def run(argv, log_path, append):
    """plat_spawn's thread: the script with its output in log_path; its exit code."""
    with open(log_path, 'a' if append else 'w', encoding='utf-8', errors='replace', buffering=1) as out:
        return _run_script(list(argv), out, out)


class _Kept(io.BytesIO):  # (a script's `with sys.stdout:` closes it: what was written stays)
    def close(self): pass


class Popen:
    """subprocess.Popen for `[sys.executable, <script> | -m <module> | -c <code>, args...]` (see the top)."""
    def __init__(self, args, bufsize=-1, executable=None, stdin=None, stdout=None, stderr=None, preexec_fn=None,
                 close_fds=True, shell=False, cwd=None, env=None, universal_newlines=None, startupinfo=None,
                 creationflags=0, restore_signals=True, start_new_session=False, pass_fds=(), *, text=None,
                 encoding=None, errors=None, **kw):
        self.args = args
        argv = [os.fsdecode(a) for a in ([args] if isinstance(args, (str, bytes, os.PathLike)) else args)]
        i, kind = 1, 'path'
        while i < len(argv) and argv[i][:1] == '-' and argv[i] not in ('-c', '-m', '-'):
            i += 2 if argv[i] in ('-W', '-X') else 1  # (-u, -B, -I...: nothing for them to change here)
        if i < len(argv) and argv[i] in ('-c', '-m'):
            kind = 'code' if argv[i] == '-c' else 'module'
            i += 1
        if shell or not _proc() or (executable or argv[0]) != sys.executable or i >= len(argv) or argv[i] == '-':
            raise OSError(errno.ENOTSUP, f'{sys.platform} does not support processes (only sys.executable with a '
                                         'script, -m or -c runs, in place)')
        self.pid = id(self) & 0x7fffffff
        self.returncode = self.stdin = self.stdout = self.stderr = None
        self._text = bool(text or universal_newlines or encoding or errors)
        self._enc = (encoding or 'utf-8', errors or 'strict')
        outs = []
        for to, ours in ((stdout, sys.stdout), (stderr, sys.stderr)):
            if to is None: outs.append(ours)
            elif to == subprocess.STDOUT: outs.append(outs[0])
            elif to == subprocess.PIPE: outs.append(io.TextIOWrapper(_Kept(), 'utf-8', 'replace', write_through=True))
            else:
                if to != subprocess.DEVNULL and not isinstance(to, int):
                    to.flush()
                    to = to.fileno()
                outs.append(open(os.devnull if to == subprocess.DEVNULL else to, 'w', encoding='utf-8',
                                 errors='replace', buffering=1, closefd=to == subprocess.DEVNULL))
        self._outs = outs
        self._start = lambda stdin: _run_script(argv[i:], outs[0], outs[1], kind, env, cwd, stdin)
        if stdin != subprocess.PIPE:  # (with PIPE the child starts in communicate(), which brings its input)
            self._finish(None if stdin is None else '')

    def _finish(self, input):
        if self.returncode is not None:
            return
        if isinstance(input, bytes):
            input = input.decode(*self._enc)
        self.returncode = self._start(None if input is None else io.StringIO(input))
        for name, o in zip(('stdout', 'stderr'), self._outs):
            if isinstance(getattr(o, 'buffer', None), _Kept) and (name == 'stdout' or o is not self._outs[0]):
                raw = io.BytesIO(o.buffer.getvalue())
                setattr(self, name, io.TextIOWrapper(raw, *self._enc) if self._text else raw)

    def communicate(self, input=None, timeout=None):
        self._finish(input or '')
        return (self.stdout.read() if self.stdout else None, self.stderr.read() if self.stderr else None)

    def wait(self, timeout=None):
        self._finish('')
        return self.returncode

    poll = wait

    def kill(self): pass
    terminate = kill

    def send_signal(self, sig): pass

    def __enter__(self): return self

    def __exit__(self, *exc):
        self._finish('')


def install(fresh_dir):
    """Once, as the interpreter starts. fresh_dir: the program whose modules each run imports anew."""
    global _fresh_dir
    _fresh_dir = fresh_dir
    _real.update(chdir=os.chdir, putenv=os.putenv, unsetenv=os.unsetenv)
    os.chdir = os.fchdir = _chdir
    os.putenv, os.unsetenv = _putenv, _unsetenv
    # (every way a thread starts: threading's own names for them differ between Python versions)
    for mod, name in ((_thread, 'start_new_thread'), (_thread, 'start_joinable_thread'),
                      (threading, '_start_new_thread'), (threading, '_start_joinable_thread')):
        if hasattr(mod, name):
            setattr(mod, name, _starter(getattr(mod, name)))
    subprocess.Popen = Popen

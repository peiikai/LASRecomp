// iOS: an embedded CPython (BeeWare's Python.xcframework, tools/build_deps_ios.sh) stands in for the seed
// generator process - iOS apps can't start one. The interpreter lives in <app>/python (the stdlib; its C extensions
// as Frameworks/<module>.framework, the stdlib's .fwork files point there) with the generator's packages in
// <app>/python/site-packages (oead, keystone, yaml, evfl, appdirs) and the generator itself (tools/rando,
// LAS-Randomizer) in <app>/rando-gen.
//
// plat_spawn (runtime/platform.h) here runs a Python script as a thread that behaves like the process the other
// platforms start: argv[0] ends in .py, the rest is its sys.argv, its stdout / stderr go to log_path, sys.exit(n) is
// its exit code (an uncaught exception: 1, with the traceback in the log), plat_proc_wait / plat_proc_kill work on
// it. A fresh start each time, as a process would have: each run is a sub-interpreter of its own (Py_NewInterpreter,
// ended with the run), so no module, class attribute or counter of one run is seen by the next.
#import <Foundation/Foundation.h>
#include <Python.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../platform.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_up;
static char g_error[512];
static PyThreadState *g_main_state;

static bool init_python(void) {
    if (g_up) return true;
    const char *bundle = plat_ios_bundle_dir();
    char home[1100];
    snprintf(home, sizeof home, "%s/python", bundle);

    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    PyStatus st = Py_PreInitialize(&pre);
    if (PyStatus_Exception(st)) { snprintf(g_error, sizeof g_error, "preinit: %s", st.err_msg); return false; }

    PyConfig cfg;
    PyConfig_InitIsolatedConfig(&cfg);
    cfg.buffered_stdio = 0;
    cfg.write_bytecode = 0;  // (the bundle is signed, read-only)
    cfg.install_signal_handlers = 0;  // (the runtime's crash handler stays)
    cfg.use_hash_seed = 1;  // (PYTHONHASHSEED=0 on the other platforms: set iteration order must not differ
    cfg.hash_seed = 0;      //  between runs - same seed ID, same seed)
    wchar_t *w = Py_DecodeLocale(home, NULL);
    st = PyConfig_SetString(&cfg, &cfg.home, w);
    PyMem_RawFree(w);
    if (!PyStatus_Exception(st)) {
        // sys.path: the stdlib, its extensions, the generator's packages and sources (set explicitly: the
        // interpreter's own computation of the stdlib path is replaced as a whole)
        char p[4][1100];
        snprintf(p[0], sizeof p[0], "%s/python/lib/python" LARECOMP_PY_VERSION, bundle);
        snprintf(p[1], sizeof p[1], "%s/python/lib/python" LARECOMP_PY_VERSION "/lib-dynload", bundle);
        snprintf(p[2], sizeof p[2], "%s/python/site-packages", bundle);
        snprintf(p[3], sizeof p[3], "%s/rando-gen", bundle);
        for (int i = 0; i < 4 && !PyStatus_Exception(st); i++) {
            w = Py_DecodeLocale(p[i], NULL);
            st = PyWideStringList_Append(&cfg.module_search_paths, w);
            PyMem_RawFree(w);
        }
        cfg.module_search_paths_set = 1;
    }
    if (!PyStatus_Exception(st)) st = PyConfig_Read(&cfg);
    if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&cfg);
    PyConfig_Clear(&cfg);
    if (PyStatus_Exception(st)) { snprintf(g_error, sizeof g_error, "init: %s", st.err_msg); return false; }
    // keystone's library is not a Python extension: keystone.py loads it with ctypes by name, from its own folder
    // first. A symlink there to the framework doesn't survive an iOS install (sideloading tools drop symlinks from
    // the .ipa), so the framework is loaded here and dyld then answers `libkeystone.dylib` with this image (its
    // install name's leaf) - no file in site-packages/keystone needed.
    char ks[1200];
    snprintf(ks, sizeof ks, "%s/Frameworks/keystone.libkeystone.framework/keystone.libkeystone", bundle);
    if (!dlopen(ks, RTLD_NOW | RTLD_GLOBAL)) fprintf(stderr, "[py] keystone: %s\n", dlerror());
    // Python's stdout / stderr by default: our stderr, the log (its own are the process's original fds, which the
    // session log replaced with freopen - a different FILE). A script run as a "process" gets its own log file.
    PyRun_SimpleString("import sys, os\n"
                       "class _FdOut:\n"
                       "    def __init__(self, fd): self.fd = fd\n"
                       "    def write(self, s):\n"
                       "        os.write(self.fd, s.encode('utf-8', 'replace')); return len(s)\n"
                       "    def flush(self): pass\n"
                       "    def isatty(self): return False\n"
                       "sys.stdout = sys.stderr = _FdOut(2)\n");
    g_main_state = PyEval_SaveThread();  // (the GIL is free: the scripts run on their own threads)
    fprintf(stderr, "[py] python %s ready\n", Py_GetVersion());
    g_up = true;
    return true;
}

// Runs Python source (on the calling thread, under the GIL; stdout / stderr to the log). 0 = ok; else the
// exception's text in err.
int plat_ios_python_run(const char *source, char *err, size_t err_size) {
    pthread_mutex_lock(&g_lock);
    bool up = init_python();
    pthread_mutex_unlock(&g_lock);
    if (!up) { snprintf(err, err_size, "%s", g_error); return 1; }
    PyGILState_STATE gs = PyGILState_Ensure();
    PyObject *main = PyImport_AddModule("__main__");
    PyObject *globals = PyModule_GetDict(main);
    PyObject *r = PyRun_String(source, Py_file_input, globals, globals);
    int rc = 0;
    if (!r) {
        PyObject *type, *value, *tb;
        PyErr_Fetch(&type, &value, &tb);
        PyErr_NormalizeException(&type, &value, &tb);
        PyObject *s = value ? PyObject_Str(value) : NULL;
        snprintf(err, err_size, "%s: %s", type ? ((PyTypeObject *)type)->tp_name : "error", s ? PyUnicode_AsUTF8(s) : "");
        Py_XDECREF(s);
        PyErr_Restore(type, value, tb);
        PyErr_Print();
        rc = 1;
    } else {
        Py_DECREF(r);
    }
    PyGILState_Release(gs);
    return rc;
}

// ------------------------------------------------------------------ scripts as processes
#define MAX_PROCS 8
typedef struct {
    bool used;
    pthread_t thread;
    _Atomic int exit_code;        // -1 while running
    _Atomic unsigned long tid;    // Python's thread id (for the interrupt), 0 before it runs
    PyInterpreterState *interp;   // its sub-interpreter while it runs (read and written under the GIL)
    char **argv;                  // script path first
    char *log;
    bool append;
} PyProc;
static PyProc g_procs[MAX_PROCS];

// The runner, in Python: the script's "process" environment, then runpy as __main__. Returns the exit code.
static const char *RUNNER =
    "import sys, os, runpy, traceback\n"
    "class _FdOut:\n"
    "    def __init__(self, fd): self.fd = fd\n"
    "    def write(self, s):\n"
    "        os.write(self.fd, s.encode('utf-8', 'replace')); return len(s)\n"
    "    def flush(self): pass\n"
    "    def isatty(self): return False\n"
    "def _run(argv, log_path, append):\n"
    "    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | (os.O_APPEND if append else os.O_TRUNC), 0o644)\n"
    "    out = _FdOut(fd)\n"
    "    cwd = os.getcwd()\n"
    "    sys.argv, sys.stdout, sys.stderr = list(argv), out, out\n"
    "    code = 0\n"
    "    try:\n"
    "        runpy.run_path(argv[0], run_name='__main__')\n"
    "    except SystemExit as e:\n"
    "        c = e.code\n"
    "        if c is None: code = 0\n"
    "        elif isinstance(c, int): code = c\n"
    "        else: out.write(str(c) + '\\n'); code = 1\n"
    "    except KeyboardInterrupt:\n"
    "        out.write('killed\\n'); code = 137\n"
    "    except BaseException:\n"
    "        out.write(traceback.format_exc()); code = 1\n"
    "    finally:\n"
    "        sys.stdout = sys.stderr = _FdOut(2)\n"
    "        try: os.chdir(cwd)\n"  // (the working directory is the process's: the data root)
    "        except OSError: pass\n"
    "        os.close(fd)\n"
    "    return code\n";

static void *proc_main(void *arg) {
    PyProc *p = arg;
    PyGILState_STATE gs = PyGILState_Ensure();
    PyThreadState *main_ts = PyThreadState_Get();
    PyThreadState *sub = Py_NewInterpreter();  // (the legacy kind: shared GIL, the main config's sys.path / hash seed)
    int code = 1;
    if (!sub) {
        PyThreadState_Swap(main_ts);
        fprintf(stderr, "[py] no sub-interpreter for %s\n", p->argv[0]);
    } else {
        p->interp = PyThreadState_GetInterpreter(sub);
        atomic_store(&p->tid, PyThread_get_thread_ident());
        PyObject *globals = PyModule_GetDict(PyImport_AddModule("__main__"));
        PyObject *r = PyRun_String(RUNNER, Py_file_input, globals, globals);
        Py_XDECREF(r);
        PyObject *run = PyDict_GetItemString(globals, "_run");
        if (run) {
            int n = 0;
            while (p->argv[n]) n++;
            PyObject *argv = PyList_New(n);
            for (int i = 0; i < n; i++) PyList_SET_ITEM(argv, i, PyUnicode_FromString(p->argv[i]));
            r = PyObject_CallFunction(run, "OsO", argv, p->log, p->append ? Py_True : Py_False);
            if (r) { code = (int)PyLong_AsLong(r); Py_DECREF(r); }
            else PyErr_Print();
            Py_DECREF(argv);
        } else {
            PyErr_Print();
        }
        p->interp = NULL;
        Py_EndInterpreter(sub);
        PyThreadState_Swap(main_ts);
    }
    PyGILState_Release(gs);
    atomic_store(&p->exit_code, code);
    return NULL;
}

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    size_t l = strlen(argv[0]);
    if (l < 3 || strcmp(argv[0] + l - 3, ".py")) return 0;  // (only Python scripts run here: no other programs)
    pthread_mutex_lock(&g_lock);
    bool up = init_python();
    PyProc *p = NULL;
    if (up)
        for (int i = 0; i < MAX_PROCS; i++)
            if (!g_procs[i].used) { p = &g_procs[i]; break; }
    if (p) {
        memset(p, 0, sizeof *p);
        p->used = true;
        atomic_store(&p->exit_code, -1);
        int n = 0;
        while (argv[n]) n++;
        p->argv = calloc(n + 1, sizeof *p->argv);
        for (int i = 0; i < n; i++) p->argv[i] = strdup(argv[i]);
        p->log = strdup(log_path ? log_path : "/dev/null");
        p->append = append_log;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16 << 20);  // (the generator's logic recurses)
        if (pthread_create(&p->thread, &at, proc_main, p)) { p->used = false; p = NULL; }
        pthread_attr_destroy(&at);
    }
    pthread_mutex_unlock(&g_lock);
    if (!up) fprintf(stderr, "[py] %s\n", g_error);
    return p ? (PlatProc)(p - g_procs + 1) : 0;
}

static void proc_free(PyProc *p) {
    for (int i = 0; p->argv && p->argv[i]; i++) free(p->argv[i]);
    free(p->argv);
    free(p->log);
    p->used = false;
}

int plat_proc_wait(PlatProc h, bool block) {
    if (h < 1 || h > MAX_PROCS) return 255;
    PyProc *p = &g_procs[h - 1];
    if (!p->used) return 255;
    int code = atomic_load(&p->exit_code);
    if (code < 0 && !block) return -1;
    pthread_join(p->thread, NULL);
    code = atomic_load(&p->exit_code);
    proc_free(p);
    return code;
}

void plat_proc_kill(PlatProc h) {
    if (h < 1 || h > MAX_PROCS) return;
    PyProc *p = &g_procs[h - 1];
    if (!p->used) return;
    unsigned long tid = atomic_load(&p->tid);
    if (atomic_load(&p->exit_code) < 0 && tid) {  // KeyboardInterrupt at its next bytecode (the runner reports 137)
        PyGILState_STATE gs = PyGILState_Ensure();
        if (p->interp) {  // (SetAsyncExc looks in the current interpreter's threads: a thread state of the run's)
            PyThreadState *t = PyThreadState_New(p->interp), *prev = PyThreadState_Swap(t);
            PyThreadState_SetAsyncExc(tid, PyExc_KeyboardInterrupt);
            PyThreadState_Clear(t);
            PyThreadState_Swap(prev);
            PyThreadState_Delete(t);
        }
        PyGILState_Release(gs);
    }
    pthread_join(p->thread, NULL);
    proc_free(p);
}

// Smoke test (LARECOMP_IOS_PYTEST=1 at startup, ios.c plat_init): every native dependency of the seed
// generator, imported and exercised once.
void plat_ios_python_selftest(void) {
    static const char *src =
        "import sys, time\n"
        "t0 = time.time()\n"
        "print('[py] python', sys.version.split()[0], 'home', sys.prefix)\n"
        "import oead, keystone, yaml, evfl, appdirs\n"
        "import quicktex, quicktex.s3tc.bc1, PIL.Image\n"
        "img = PIL.Image.new('RGBA', (4, 4), (200, 100, 50, 255))\n"
        "blk = quicktex.s3tc.bc1.BC1Encoder().encode(quicktex.RawTexture.frombytes(img.tobytes(), 4, 4))\n"
        "print('[py] quicktex bc1', blk.nbytes, 'bytes, pillow', PIL.__version__)\n"
        "ks = keystone.Ks(keystone.KS_ARCH_ARM64, keystone.KS_MODE_LITTLE_ENDIAN)\n"
        "code, n = ks.asm('mov x0, #1; add x1, x0, #2; ret')\n"
        "print('[py] keystone', n, 'insns ->', bytes(code).hex())\n"
        "w = oead.SarcWriter(oead.Endianness.Little)\n"
        "w.files['a.txt'] = b'hello'\n"
        "data = w.write()[1]\n"
        "s = oead.Sarc(data)\n"
        "print('[py] oead sarc', [f.name for f in s.get_files()], bytes(s.get_file('a.txt').data))\n"
        "z = oead.yaz0.compress(b'x' * 1000)\n"
        "print('[py] yaz0', len(z), len(oead.yaz0.decompress(z)))\n"
        "print('[py] yaml', yaml.safe_load('a: [1, 2]'))\n"
        "print('[py] selftest ok in %.0f ms' % ((time.time() - t0) * 1000))\n";
    char err[512];
    if (plat_ios_python_run(src, err, sizeof err)) { fprintf(stderr, "[py] selftest FAILED: %s\n", err); return; }
    // then the generator itself as a "process", on a default seed, like the Rando tab does (Documents/pytest-seed/),
    // and once more into pytest-seed2/: the second run is a second sub-interpreter in the same process, and must give
    // the same files (state left in a C extension across interpreters would show here)
    char romfs[1100];
    snprintf(romfs, sizeof romfs, "%s/extracted/romfs", plat_ios_documents());
    struct stat st;
    if (stat(romfs, &st) || !S_ISDIR(st.st_mode)) {  // (the CI simulator has no game files: the imports were the test)
        fprintf(stderr, "[py] generator skipped: no %s\n", romfs);
        return;
    }
    const char *names[] = {"pytest-seed", "pytest-seed2"};
    for (int i = 0; i < 2; i++) {
        char script[1100], dir[1100], req[1100], log[1100];
        snprintf(script, sizeof script, "%s/rando-gen/las_headless.py", plat_ios_bundle_dir());
        snprintf(dir, sizeof dir, "%s/%s", plat_ios_documents(), names[i]);
        snprintf(req, sizeof req, "%s/request.json", dir);
        snprintf(log, sizeof log, "%s/generate.log", dir);
        // a fresh folder, as the Rando tab's per-seed ones are: some outputs (flags_ext) are appended to
        [[NSFileManager defaultManager] removeItemAtPath:@(dir) error:nil];
        mkdir(dir, 0755);
        FILE *f = fopen(req, "w");
        if (!f) return;
        fputs("{\"seed\": \"pytest\", \"create-spoiler\": true}\n", f);  // (the rest: the generator's defaults)
        fclose(f);
        const char *argv[] = {script, romfs, dir, req, NULL};
        uint64_t t0 = plat_now_ns();
        PlatProc p = plat_spawn(argv, log, false);
        int rc = p ? plat_proc_wait(p, true) : 255;
        fprintf(stderr, "[py] generator exit %d in %llu ms (log: %s)\n", rc, (unsigned long long)((plat_now_ns() - t0) / 1000000), log);
        if (rc) return;
    }
    static const char *cmp =
        "import hashlib, os\n"
        "def tree(root):\n"
        "    d = {}\n"
        "    for dp, _, fs in os.walk(root):\n"
        "        for f in fs:\n"
        "            rel = os.path.relpath(os.path.join(dp, f), root)\n"
        "            if rel in ('generate.log', 'request.json'): continue\n"
        "            data = open(os.path.join(dp, f), 'rb').read()\n"
        "            if rel == 'spoiler_view.txt': data = data.split(b'\\n', 1)[1]  # (its first line names the folder)\n"
        "            d[rel] = hashlib.sha256(data).hexdigest()\n"
        "    return d\n"
        "a, b = tree(DOCS + '/pytest-seed'), tree(DOCS + '/pytest-seed2')\n"
        "diff = sorted(set(a) ^ set(b) | {k for k in a if k in b and a[k] != b[k]})\n"
        "print('[py] second run:', len(a), 'files,', 'identical' if not diff else 'DIFFERENT: ' + ' '.join(diff[:10]))\n";
    char pre[1300];
    snprintf(pre, sizeof pre, "DOCS = r'''%s'''\n", plat_ios_documents());
    char *src2 = malloc(strlen(pre) + strlen(cmp) + 1);
    strcpy(src2, pre);
    strcat(src2, cmp);
    if (plat_ios_python_run(src2, err, sizeof err)) fprintf(stderr, "[py] second-run compare FAILED: %s\n", err);
    free(src2);

    // the Randomizer tab's Cancel: a third run killed a second in, then a small script in a new interpreter after it
    char script[1100], dir[1100], req[1100], log[1100], after[1100];
    snprintf(script, sizeof script, "%s/rando-gen/las_headless.py", plat_ios_bundle_dir());
    snprintf(dir, sizeof dir, "%s/pytest-kill", plat_ios_documents());
    snprintf(req, sizeof req, "%s/request.json", dir);
    snprintf(log, sizeof log, "%s/generate.log", dir);
    snprintf(after, sizeof after, "%s/after.py", dir);
    [[NSFileManager defaultManager] removeItemAtPath:@(dir) error:nil];
    mkdir(dir, 0755);
    FILE *f = fopen(req, "w");
    if (f) fputs("{\"seed\": \"pytest\", \"create-spoiler\": true}\n", f), fclose(f);
    f = fopen(after, "w");
    if (f) fputs("import sys\nprint('PySide6' in sys.modules)\nsys.exit(3)\n", f), fclose(f);
    const char *argv[] = {script, romfs, dir, req, NULL};
    PlatProc p = plat_spawn(argv, log, false);
    usleep(1000 * 1000);
    uint64_t t0 = plat_now_ns();
    plat_proc_kill(p);
    unsigned long long kill_ms = (plat_now_ns() - t0) / 1000000;
    bool killed = false;
    if ((f = fopen(log, "r"))) {
        char line[512];
        while (fgets(line, sizeof line, f)) killed |= !strcmp(line, "killed\n");
        fclose(f);
    }
    const char *argv2[] = {after, NULL};
    char log2[1100];
    snprintf(log2, sizeof log2, "%s/after.log", dir);
    PlatProc q = plat_spawn(argv2, log2, false);
    int rc = q ? plat_proc_wait(q, true) : 255;
    char out[16] = "";
    if ((f = fopen(log2, "r"))) { if (!fgets(out, sizeof out, f)) out[0] = 0; fclose(f); }
    bool fresh = !strcmp(out, "False\n");  // (the killed run's PySide6 stubs not in the next interpreter)
    fprintf(stderr, "[py] kill: %s (stopped in %llu ms, next run exit %d, %s)\n", killed && rc == 3 && fresh ? "ok" : "FAILED",
            kill_ms, rc, fresh ? "a fresh interpreter" : "state left over");
}

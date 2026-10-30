"""Native detached handoff -> wait for parent -> install -> restart, using synthetic files.

Optional argument: a frozen lasupdate.exe to exercise the packaged helper too.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

import updater as u
from test_updater import package, offer

if os.name != 'nt':
    raise SystemExit('Windows-only test')

root = Path(__file__).resolve().parent.parent
source = (root / 'runtime/platform/windows.c').read_text(encoding='utf-8')
quote = source[source.index('static void append_arg('):source.index('// No exec() on Windows:')]
detach = source[source.index('bool plat_spawn_detached('):source.index('PlatProc plat_spawn_no_cfg(')]
harness = r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "platform.h"
'''+quote+detach+r'''
int wmain(int argc, wchar_t **wide) {
    assert(argc == 7);
    char storage[7][4096], *argv[7];
    for(int i=0;i<argc;i++) { assert(WideCharToMultiByte(CP_UTF8,0,wide[i],-1,storage[i],4096,NULL,NULL));argv[i]=storage[i]; }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool deny = !strcmp(argv[6], "deny");
    if (!deny) li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    assert(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li));
    assert(AssignProcessToJobObject(job, GetCurrentProcess()));
    char pid[32]; snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    const char *av[16]; int n=0;
    av[n++]=argv[1]; if(strcmp(argv[2],"-")) av[n++]=argv[2];
    av[n++]="install"; av[n++]="--state"; av[n++]=argv[3];
    av[n++]="--target"; av[n++]=argv[4]; av[n++]="--parent"; av[n++]=pid; av[n]=NULL;
    bool started=plat_spawn_detached(av,argv[5]);
    if(deny) { assert(!started); return 0; }
    assert(started);
    Sleep(5000);  // worker must wait; the test checks the old installation during this interval
    return 0;     // its kill-on-close job must not kill the detached worker
}
'''
dummy = r'''
#include <stdio.h>
#include <windows.h>
int main(void) {
    FILE *f=fopen("restarted.txt","wb"); if(!f)return 1;fputs("new app started",f);fclose(f);
    wchar_t root[4096]; if(!GetEnvironmentVariableW(L"LARECOMP_DATA_ROOT",root,4096))return 2;
    char utf8[16384]; WideCharToMultiByte(CP_UTF8,0,root,-1,utf8,sizeof utf8,NULL,NULL);
    f=fopen("data-root.txt","wb"); if(!f)return 3;fputs(utf8,f);fclose(f);return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    scratch = Path(temp)
    compiler = shlex.split(os.environ.get('CC', 'clang'))
    parent_src, app_src = scratch/'parent.c', scratch/'app.c'
    parent_src.write_text(harness, encoding='utf-8'); app_src.write_text(dummy, encoding='utf-8')
    parent, app = scratch/'parent.exe', scratch/'app.exe'
    subprocess.run(compiler + ['-O1', '-municode', '-Iruntime', str(parent_src), '-o', str(parent)], cwd=root, check=True)
    subprocess.run(compiler + ['-O1', str(app_src), '-o', str(app)], check=True)
    state, target = scratch/'data'/ 'cache'/'updater', scratch/'App with spaces Ü'
    state.mkdir(parents=True); target.mkdir()
    (target/'lasrecomp.exe').write_bytes(app.read_bytes())
    (target/'save').mkdir(); (target/'save'/'progress').write_bytes(b'player save')
    payload = package({'lasrecomp.exe': app.read_bytes()})
    (state/'package.zip').write_bytes(payload)
    u.atomic_json(state/'offer.json', offer(payload=payload))
    worker = [str(Path(sys.argv[1]).resolve()), '-'] if len(sys.argv)>1 else [sys.executable, str(root/'tools/updater.py')]
    argv = [str(parent), *worker, str(state), str(target), str(state/'update.log')]
    subprocess.run([*argv, 'deny'], check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    assert not (state/'result.txt').exists(), 'restricted job must refuse the handoff'
    child = subprocess.Popen([*argv, 'allow'], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        deadline = time.monotonic()+20
        waiting_seen = False
        while time.monotonic()<deadline and child.poll() is None:
            result = (state/'result.txt').read_text() if (state/'result.txt').exists() else ''
            if result.startswith('waiting\n'):
                waiting_seen = True
                assert not (target/'restarted.txt').exists(), 'installed while parent was running'
                assert not list(target.glob('.lasrecomp-update-*')), 'modified application before exit'
            time.sleep(.05)
        assert waiting_seen, ('did not observe worker waiting for the parent', child.poll(),
            (state/'result.txt').read_text() if (state/'result.txt').exists() else '',
            (state/'update.log').read_text(errors='replace') if (state/'update.log').exists() else 'no log')
        assert child.wait(timeout=10) == 0
        deadline = time.monotonic()+30
        while time.monotonic()<deadline and not (target/'restarted.txt').exists(): time.sleep(.05)
        assert (target/'restarted.txt').read_text() == 'new app started'
        deadline = time.monotonic()+5
        while time.monotonic()<deadline and not (target/'data-root.txt').exists(): time.sleep(.05)
        assert Path((target/'data-root.txt').read_text(encoding='utf-8')) == state.parent.parent
        assert (target/'save'/'progress').read_bytes() == b'player save'
        assert (state/'result.txt').read_text().startswith('installed\n')
        # The one-file bootloader still owns the inherited log briefly after its Python worker finishes.
        deadline = time.monotonic()+10
        while True:
            try:
                (state/'update.log').unlink()
                break
            except PermissionError:
                if time.monotonic() >= deadline: raise
                time.sleep(.05)
        print('Native Windows updater: restricted job refused; detached worker survived parent job, waited, installed and restarted; save preserved')
    finally:
        if child.poll() is None: child.kill(); child.wait()

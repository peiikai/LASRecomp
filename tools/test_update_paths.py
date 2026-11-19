#!/usr/bin/env python3
# Update paths across every shipped version: builds each one's OWN update check (its tag's launcher.mm /
# update_tags.h, from git) and runs it on the live releases list, then on planned releases added to it. Use it before
# changing tags, file names or which files a release carries. Needs git tags, clang++ and the network (or a saved
# releases JSON: tools/test_update_paths.py releases.json).
# Run from the repo root: python3 tools/test_update_paths.py
import copy, json, os, re, subprocess, sys, tempfile, urllib.request

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=True).stdout.strip()
TMP = tempfile.mkdtemp(prefix='update-paths-')


def show(rev, path):
    return subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:{path}'], capture_output=True, text=True).stdout


# one checker per shipped build: its tag_rank (launcher.mm up to Beta 5.2, update_tags.h after) and its decision block
# ("std::string best;" up to the log line), with the defines the shipped build had
def build(name, rev, defines):
    src = show(rev, 'runtime/ui/launcher.mm')
    tags = show(rev, 'runtime/ui/update_tags.h')
    d = os.path.join(TMP, name)
    os.makedirs(d)
    if tags:
        open(os.path.join(d, 'update_tags.h'), 'w').write(tags)
        head = '#include "update_tags.h"\n'
    else:
        head = re.search(r'static long long tag_rank\(.*?\n}\n', src, re.S).group(0)
    block = re.search(r'\n    std::string best;\n.*?\n(?=    if \(!best\.empty\(\)\))', src, re.S).group(0)
    base = {k: m.group(1) for k in ('LARECOMP_VERSION', 'LARECOMP_TEST_OF')
            if (m := re.search(r'^#define ' + k + r' (".*?")', src, re.M))}
    base.setdefault('LARECOMP_TEST_OF', '""')
    defs = ''.join(f'#define {k} {v}\n' for k, v in {**base, **defines}.items())
    open(os.path.join(d, 'check.cpp'), 'w').write(f'''#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <string>
{defs}{head}
int main(int argc, char **argv) {{
    std::string body; FILE *f = fopen(argv[1], "rb"); char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n); fclose(f);
{block}
    printf("%s\\n", best.empty() ? "-" : best.c_str());
}}
''')
    exe = os.path.join(d, 'check')
    subprocess.run(['clang++', '-std=c++17', '-O1', '-w', os.path.join(d, 'check.cpp'), '-o', exe], check=True)
    return exe


Q = lambda s: f'"{s}"'
A64 = {'LARECOMP_PLATFORM': Q('linux-aarch64'), 'LARECOMP_EXPERIMENTAL_UNTIL': '""'}
# (Beta 5.x: no platform logic, the same answer everywhere. v0.6.0's linux-aarch64 build carries an experimental stamp,
# release.yml's run of 2026-11-15 05:29 UTC; its other desktop builds none.)
BUILDS = {f'{t} (any platform)': (t, {}) for t in ('beta_5', 'beta_5.1', 'beta_5.2', 'beta_5.3', 'beta_5.4')}
BUILDS['v0.6.0 linux-aarch64'] = ('beta_6', {**A64, 'LARECOMP_EXPERIMENTAL_STAMP': '202611150529LL'})
BUILDS['v0.6.0 other desktops'] = ('beta_6', {'LARECOMP_PLATFORM': Q('linux-x86_64'), 'LARECOMP_EXPERIMENTAL_UNTIL': '""',
                                              'LARECOMP_EXPERIMENTAL_STAMP': '0LL'})
NIGHTLY_REV = 'origin/develop'  # (a nightly build's update check: develop's code with its stamp)


def nightly_build(stamp, commit):
    return (NIGHTLY_REV, {**A64, 'LARECOMP_VERSION': Q(f'nightly-{stamp}-{commit}'), 'LARECOMP_TEST_OF': '""',
                          'LARECOMP_EXPERIMENTAL_STAMP': '0LL', 'LARECOMP_NIGHTLY_STAMP': f'{stamp}LL'})


PLATS = ['macOS.dmg', 'windows-x64.zip', 'linux-x86_64.AppImage', 'android.apk', 'iOS.laskit']  # (windows.zip before 2026-11)


def release(tag, aarch64):
    files = [f'LAS.Recomp-{tag}-{p}' for p in PLATS] + ([f'LAS.Recomp-{tag}-linux-aarch64.AppImage'] if aarch64 else [])
    return {'tag_name': tag, 'name': tag, 'prerelease': False, 'draft': False, 'assets': [{'name': n} for n in files]}


def nightly_round(rels, stamp, commit):
    rels = copy.deepcopy(rels)
    for r in rels:
        if r['tag_name'] == 'nightly':
            r['assets'] = [{'name': f'LAS.Recomp-nightly-{stamp}-{commit}-{p}'}
                           for p in PLATS + ['linux-aarch64.AppImage', 'windows-arm64.zip', 'windows.zip']]  # (+ the handover copy)
    return rels


def main():
    if len(sys.argv) > 1:
        live = json.load(open(sys.argv[1]))
    else:
        rq = urllib.request.Request('https://api.github.com/repos/peiikai/LASRecomp/releases?per_page=100',
                                    headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'LASRecomp/test'})
        live = json.load(urllib.request.urlopen(rq, timeout=30))
    # the linux-aarch64 nightly there now: what a release's linux-aarch64 file is a copy of (release.yml)
    names = [a['name'] for r in live if r['tag_name'] == 'nightly' for a in r['assets']]
    m = max((re.match(r'LAS\.Recomp-nightly-(\d{12})-([0-9a-f]{7})-linux-aarch64\.AppImage$', n) for n in names
             if n.endswith('-linux-aarch64.AppImage')), key=lambda m: m.group(1) if m else '', default=None)
    builds = dict(BUILDS)
    if m:
        builds[f'release aarch64 file = nightly {m.group(1)}'] = nightly_build(m.group(1), m.group(2))
    # (the Experimental page holds nightly builds renamed for old experimental builds: v0.6.0 linux-aarch64 takes one)
    for r in live:
        for a in r['assets'] if r['tag_name'] == 'experimental' else []:
            if e := re.match(r'LAS\.Recomp-experimental-(\d{12})-([0-9a-f]{7})-linux-aarch64\.AppImage$', a['name']):
                builds[f'Experimental page file = nightly {e.group(1)}'] = nightly_build(e.group(1), e.group(2))
    exes = {n: build(f'b{i}', *b) for i, (n, b) in enumerate(builds.items())}

    later = '209912312359'
    scen = {'live releases': live}
    scen['+ v0.6.1 (aarch64 file = nightly copy)'] = [release('v0.6.1', True)] + live
    scen['+ a newer nightly round'] = nightly_round(scen['+ v0.6.1 (aarch64 file = nightly copy)'], later, 'abcdef0')
    scen['+ v0.6.2 with no aarch64 file'] = [release('v0.6.2', False)] + scen['+ a newer nightly round']
    scen['+ Experimental release deleted'] = [r for r in scen['+ a newer nightly round'] if r['tag_name'] != 'experimental']
    for sn, rels in scen.items():
        fn = os.path.join(TMP, 'releases.json')
        json.dump(rels, open(fn, 'w'), indent=2)  # (pretty, as the API sends it)
        print(f'== {sn}')
        for n, exe in exes.items():
            print(f'   {n:40s} -> {subprocess.run([exe, fn], capture_output=True, text=True).stdout.strip()}')


main()

#!/usr/bin/env python3
"""tools/ios_frameworks.py on a fixture app: the stdlib's and site-packages' extensions become frameworks, a .fwork
points at each, keystone's dylib gets a relative symlink (or a .symlink file for the skeleton), and the module
names / bundle ids are what CPython's importer and the signer expect."""
import os
import plistlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ios_frameworks as fw  # noqa: E402

EXT = '.cpython-313-iphoneos.so'


def fixture(root):
    app = os.path.join(root, 'LAS Recomp.app')
    files = [
        f'python/lib/python3.13/lib-dynload/_bz2{EXT}',
        f'python/lib/python3.13/lib-dynload/_decimal{EXT}',
        f'python/site-packages/oead{EXT}',
        f'python/site-packages/PIL/_imaging{EXT}',
        f'python/site-packages/quicktex/s3tc/bc1/_bc1{EXT}',
        'python/site-packages/keystone/libkeystone.dylib',
        'python/site-packages/keystone/keystone.py',  # (not an extension: stays)
        'python/lib/python3.13/os.py',
    ]
    for rel in files:
        p = os.path.join(app, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(rel.encode())
    os.makedirs(os.path.join(app, 'Frameworks'))
    return app


def check(cond, what):
    if not cond:
        sys.exit(f'FAIL: {what}')


def run(skeleton, platform='iPhoneOS'):
    with tempfile.TemporaryDirectory() as root:
        app = fixture(root)
        args = [sys.executable, fw.__file__, app, 'com.example.las', '16.0'] + (['--skeleton'] if skeleton else [])
        if platform != 'iPhoneOS':
            args += ['--platform', platform]
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
        check(out.strip() == '6', f'printed {out!r}, expected 6 frameworks')
        expect = {
            '_bz2': f'python/lib/python3.13/lib-dynload/_bz2{EXT}',
            '_decimal': f'python/lib/python3.13/lib-dynload/_decimal{EXT}',
            'oead': f'python/site-packages/oead{EXT}',
            'PIL._imaging': f'python/site-packages/PIL/_imaging{EXT}',
            'quicktex.s3tc.bc1._bc1': f'python/site-packages/quicktex/s3tc/bc1/_bc1{EXT}',
            'keystone.libkeystone': 'python/site-packages/keystone/libkeystone.dylib',
        }
        check(sorted(os.listdir(os.path.join(app, 'Frameworks'))) == sorted(m + '.framework' for m in expect),
              f"Frameworks/: {os.listdir(os.path.join(app, 'Frameworks'))}")
        for mod, rel in expect.items():
            binary = os.path.join(app, 'Frameworks', mod + '.framework', mod)
            check(os.path.isfile(binary) and open(binary, 'rb').read() == rel.encode(), f'{mod}: the binary moved')
            with open(os.path.join(app, 'Frameworks', mod + '.framework', 'Info.plist'), 'rb') as p:
                info = plistlib.load(p)
            check(info['CFBundleExecutable'] == mod and info['MinimumOSVersion'] == '16.0', f'{mod}: Info.plist')
            check(info['CFBundleSupportedPlatforms'] == [platform], f"{mod}: platforms {info['CFBundleSupportedPlatforms']}")
            check(info['CFBundleIdentifier'] == 'com.example.las.' + mod.replace('_', '-'), f"{mod}: id {info['CFBundleIdentifier']}")
            check('_' not in info['CFBundleIdentifier'], f'{mod}: underscore in the bundle id')
            src = os.path.join(app, rel)
            if rel.endswith('.so'):
                check(not os.path.exists(src), f'{rel} still there')
                pointer = src[:-len('.so')] + '.fwork'
                check(open(pointer).read().strip() == f'Frameworks/{mod}.framework/{mod}', f'{pointer}: content')
            elif skeleton:
                check(not os.path.lexists(src), f'{rel} still there (skeleton)')
                check(open(src + '.symlink').read() == f'../../../Frameworks/{mod}.framework/{mod}', f'{rel}.symlink')
            else:
                check(os.path.islink(src), f'{rel}: not a symlink')
                check(os.path.realpath(src) == os.path.realpath(binary), f'{rel}: link target {os.readlink(src)}')
        # a second run finds nothing left to move (the .fwork / symlink are not extensions)
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
        check(out.strip() == '0', f'second run moved {out.strip()} files')
        for keep in ('python/site-packages/keystone/keystone.py', 'python/lib/python3.13/os.py'):
            check(os.path.isfile(os.path.join(app, keep)), f'{keep} touched')


def bad_names():
    check(fw.module_name('python/site-packages/PIL/_imaging' + EXT) == 'PIL._imaging', 'module_name site-packages')
    check(fw.module_name('python/lib/python3.14/lib-dynload/_bz2.cpython-314-iphoneos.so') == '_bz2', 'module_name lib-dynload')
    try:
        fw.module_name('python/lib/python3.13/site-packages/x.so')
    except ValueError:
        pass
    else:
        sys.exit('FAIL: a path outside the two folders was accepted')


if __name__ == '__main__':
    bad_names()
    run(skeleton=False)
    run(skeleton=True)
    run(skeleton=False, platform='iPhoneSimulator')
    print('ok')

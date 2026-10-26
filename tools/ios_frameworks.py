#!/usr/bin/env python3
"""usage: ios_frameworks.py <app> <bundle id> <min iOS> [--platform iPhoneOS|iPhoneSimulator]

The embedded CPython's C extensions as frameworks (tools/bundle_ios.sh): iOS loads code only from the app's own
signed binary and from Frameworks/*.framework, so every .so under <app>/python/ (the stdlib's lib-dynload and
site-packages) is moved to Frameworks/<dotted.module>.framework/<dotted.module> with an Info.plist, and a
<module>.fwork file holding the framework-relative path is left in its place for CPython's iOS importer - the
layout of BeeWare's testbed (Python-Apple-support, build/utils.sh). A .dylib under python/ (keystone's, which
keystone.py loads with ctypes) gets the same framework and nothing in its place: no symlink survives an iOS
install (a sideloading tool drops it from the .ipa), so the runtime dlopens the framework before Python starts
(runtime/platform/ios_python.m) and keystone.py's `cdll.LoadLibrary('libkeystone.dylib')` finds the loaded image
by its install name's leaf. Prints the number of frameworks made.
"""
import os
import plistlib
import sys

PY = 'python'  # the app's Python folder: python/lib/python<ver>/lib-dynload, python/site-packages


def module_name(rel):
    """python/site-packages/PIL/_imaging.cpython-313-iphoneos.so -> PIL._imaging;
    python/lib/python3.13/lib-dynload/_bz2.cpython-313-iphoneos.so -> _bz2; .../keystone/libkeystone.dylib ->
    keystone.libkeystone (the first dot of the file name ends the module name: the ABI tag follows)."""
    parts = rel.split('/')
    name = parts[-1].split('.')[0]
    if parts[1] == 'site-packages':
        pkg = parts[2:-1]
    elif len(parts) > 4 and parts[1] == 'lib' and parts[3] == 'lib-dynload':
        pkg = parts[4:-1]
    else:
        raise ValueError(f'{rel}: not under python/site-packages or python/lib/python*/lib-dynload')
    return '.'.join(pkg + [name])


def info_plist(mod, bundle_id, min_ios, platform='iPhoneOS'):
    return {
        'CFBundleExecutable': mod,
        'CFBundleIdentifier': f"{bundle_id}.{mod.replace('_', '-')}",  # (no underscores in a bundle id)
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': mod,
        'CFBundlePackageType': 'FMWK',
        'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1',
        'CFBundleSupportedPlatforms': [platform],
        'MinimumOSVersion': min_ios,
    }


def make_frameworks(app, bundle_id, min_ios, platform='iPhoneOS'):
    made = {}
    for dirpath, _, files in os.walk(os.path.join(app, PY)):
        for f in sorted(files):
            if not (f.endswith('.so') or f.endswith('.dylib')):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, app)
            mod = module_name(rel)
            if mod in made:
                raise ValueError(f'{rel} and {made[mod]} both map to the framework {mod}')
            made[mod] = rel
            fw = os.path.join(app, 'Frameworks', mod + '.framework')
            os.makedirs(fw, exist_ok=True)
            os.replace(path, os.path.join(fw, mod))
            with open(os.path.join(fw, 'Info.plist'), 'wb') as p:
                plistlib.dump(info_plist(mod, bundle_id, min_ios, platform), p)
            if f.endswith('.so'):
                with open(path[:-len('.so')] + '.fwork', 'w') as w:  # (the importer follows it)
                    w.write(f'Frameworks/{mod}.framework/{mod}\n')
    return made


def main(argv):
    args = argv[1:]
    platform = 'iPhoneOS'
    if '--platform' in args:
        i = args.index('--platform')
        platform = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3 or platform not in ('iPhoneOS', 'iPhoneSimulator'):
        sys.exit(__doc__)
    made = make_frameworks(args[0], args[1], args[2], platform)
    print(len(made))


if __name__ == '__main__':
    main(sys.argv)

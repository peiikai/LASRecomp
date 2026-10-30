"""Windows release updater. No game data is read, downloaded or replaced.

Frozen as lasupdate.exe; the UI runs checks/downloads in a supervised child, and
copies this one-file helper out of the installation before handing off a restart.
"""
import argparse
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import urllib.request
import uuid
import zipfile

API = 'https://api.github.com/repos/peiikai/LASRecomp/releases?per_page=100'
RELEASES = 'https://github.com/peiikai/LASRecomp/releases/'
MAX_DOWNLOAD = 2 << 30
MAX_UNPACKED = 4 << 30
APP_DIRS = {'lasgen', 'rando-gen', 'lasupdate', 'zig', 'gamelib-src'}
APP_FILES = {'lasrecomp.exe', 'lasrecomp.dll.a', 'icon.png', 'README.md', 'LICENSE',
             'Baloo2-SemiBold.ttf', 'Baloo2-OFL.txt', 'DejaVuSans.ttf',
             'DejaVuSans-Bold.ttf', 'DejaVu-LICENSE.txt'}


def tag_rank(tag):
    """Same ordering/bounds as runtime/ui/update_tags.h; test tags never offered."""
    m = re.fullmatch(r'beta_([0-9]+)(?:\.([0-9]+))?', tag)
    if m:
        n, fix = int(m[1]), int(m[2] or 0)
        if 0 < n < 1 << 20 and 0 <= fix < 4096 and (m[2] is None or fix > 0):
            return (n << 12) + fix
    m = re.fullmatch(r'v([0-9]+)\.([0-9]+)\.([0-9]+)', tag)
    if m:
        a, b, c = map(int, m.groups())
        if a == 0 and 0 < b < 1 << 20 and c < 4096:
            return (b << 12) + c
        if 0 < a < 1 << 20 and b < 4096 and c < 4096:
            return (1 << 48) + (a << 24) + (b << 12) + c
    return -1


def atomic_json(path, value):
    tmp = path.with_suffix('.tmp')
    with tmp.open('w', encoding='utf-8') as f:
        json.dump(value, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def status(state, kind, message, tag='', progress=0):
    safe_path(state)
    # Small, line-based protocol: no remote text can add fields or ImGui formatting.
    message = ' '.join(str(message).split())[:800]
    tmp = state / 'result.tmp'
    with tmp.open('w', encoding='utf-8') as f:
        f.write(f'{kind}\n{tag}\n{progress:.4f}\n{message}\n')
    os.replace(tmp, state / 'result.txt')


@contextlib.contextmanager
def lock(state):
    safe_path(state)
    state.mkdir(parents=True, exist_ok=True)
    with (state / 'lock').open('a+b') as f:
        if os.name == 'nt':
            import msvcrt
            f.seek(0)
            f.write(b'0')
            f.flush()
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


class HTTPSOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith('https://'):
            raise ValueError('Update download redirected away from HTTPS.')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def request(url):
    req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json',
                                              'User-Agent': 'LASRecomp-updater'})
    return urllib.request.build_opener(HTTPSOnly).open(req, timeout=30)


def validate_offer(offer):
    tag, name = offer.get('tag', ''), offer.get('name', '')
    if tag_rank(tag) < 0 or name not in (f'LAS.Recomp-{tag}-windows.zip', f'LAS Recomp-{tag}-windows.zip'):
        raise ValueError('Release has no supported Windows package.')
    if offer.get('url') != RELEASES + 'download/' + tag + '/' + name.replace(' ', '%20'):
        raise ValueError('Unexpected release download URL.')
    if not re.fullmatch(r'[0-9a-f]{64}', offer.get('sha256', '')):
        raise ValueError('GitHub did not provide the package SHA-256. Use the release page instead.')
    if type(offer.get('size')) is not int or not 0 < offer['size'] <= MAX_DOWNLOAD:
        raise ValueError('Unexpected update package size.')
    return offer


def select_offer(releases, current, preview):
    rank = tag_rank(preview) - 1 if current.startswith('test-') else tag_rank(current)
    best = None
    for release in releases:
        tag = release.get('tag_name', '')
        if release.get('draft') or tag_rank(tag) <= rank:
            continue
        # Match the current release policy: numbered betas are eligible; test-* are not.
        for asset in release.get('assets', []):
            if asset.get('name') not in (f'LAS.Recomp-{tag}-windows.zip', f'LAS Recomp-{tag}-windows.zip'):
                continue
            digest = asset.get('digest') or ''
            offer = dict(tag=tag, name=asset['name'], url=asset.get('browser_download_url', ''),
                         size=asset.get('size'), sha256=digest.removeprefix('sha256:'))
            validate_offer(offer)
            best, rank = offer, tag_rank(tag)
    return best


def check(state, current, preview):
    status(state, 'checking', 'Checking GitHub releases...')
    with request(API) as response:
        body = response.read((8 << 20) + 1)
    if len(body) > 8 << 20:
        raise ValueError('Release response is too large.')
    releases = json.loads(body)
    if not isinstance(releases, list):
        raise ValueError('Unexpected response from GitHub. Try again later.')
    offer = select_offer(releases, current, preview)
    atomic_json(state / 'offer.json', offer)
    status(state, 'available' if offer else 'current',
           'A new LAS Recomp release is available.' if offer else 'No newer Windows package is available.',
           offer['tag'] if offer else '')


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def load_offer(state):
    return validate_offer(json.loads((state / 'offer.json').read_text(encoding='utf-8')))


def newer_offer(state, current, preview):
    offer = load_offer(state)
    rank = tag_rank(preview) - 1 if current.startswith('test-') else tag_rank(current)
    if rank < 0 or tag_rank(offer['tag']) <= rank:
        raise ValueError('This cached update is not newer than the installed build. Check for updates again.')
    return offer


def download(state, current='beta_6', preview='beta_6'):
    offer = newer_offer(state, current, preview)
    partial, package = state / 'package.part', state / 'package.zip'
    status(state, 'downloading', 'Downloading the Windows release...', offer['tag'])
    try:
        total = 0
        with request(offer['url']) as response, partial.open('wb') as f:
            while chunk := response.read(1 << 20):
                total += len(chunk)
                if total > offer['size']:
                    raise ValueError('Downloaded package exceeds its published size.')
                f.write(chunk)
                status(state, 'downloading', 'Downloading the Windows release...', offer['tag'], total / offer['size'])
            f.flush()
            os.fsync(f.fileno())
        if total != offer['size'] or sha256(partial) != offer['sha256']:
            raise ValueError('Update integrity check failed. The installation was not changed.')
        # Validate the archive before offering Install; never extract into the live application.
        with zipfile.ZipFile(partial) as archive:
            package_entries(archive)
            bad = archive.testzip()
            if bad:
                raise ValueError('Update archive is damaged.')
        os.replace(partial, package)
        status(state, 'ready', 'Download verified. Save your game before installing.', offer['tag'], 1)
    finally:
        partial.unlink(missing_ok=True)


def linked(path):
    """Windows junctions/reparse points as well as POSIX symlinks."""
    st = path.lstat()
    return stat.S_ISLNK(st.st_mode) or bool(getattr(st, 'st_file_attributes', 0) & 0x400)


def safe_path(path):
    for p in (path, *path.parents):
        try:
            is_link = linked(p)
        except FileNotFoundError:
            continue
        if is_link:
            raise ValueError('The update path contains a link or junction. Use the release page instead.')


def allowed_root(name):
    return name in APP_DIRS or name in APP_FILES or (
        name.casefold() != 'lasgame.dll' and bool(re.fullmatch(r'[A-Za-z0-9_+.-]+\.dll', name)))


def package_entries(archive):
    infos = archive.infolist()
    if len(infos) > 100000 or sum(i.file_size for i in infos) > MAX_UNPACKED:
        raise ValueError('Update archive is too large.')
    seen, roots, files = set(), set(), []
    for info in infos:
        raw_name = info.orig_filename  # zipfile normalizes Windows backslashes and truncates at NUL
        raw_parts = raw_name.rstrip('/').split('/')
        parts = PurePosixPath(info.filename).parts
        if (not parts or parts[0] != 'LAS Recomp' or '\\' in raw_name or '\x00' in raw_name or
                any(p in ('', '.', '..') or ':' in p or p.endswith((' ', '.')) for p in raw_parts) or
                info.filename.startswith('/') or stat.S_ISLNK(info.external_attr >> 16) or info.flag_bits & 1):
            raise ValueError('Unsafe path in update archive.')
        if len(parts) == 1 and info.is_dir():
            continue
        if len(parts) < 2 or not allowed_root(parts[1]):
            raise ValueError('Update archive contains files outside the application package.')
        if len(parts) > 2 and parts[1] not in APP_DIRS:
            raise ValueError('Unexpected directory in update archive.')
        # Windows aliases, devices and case-insensitive duplicates must not target the same file.
        for part in parts[1:]:
            if re.match(r'(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)', part) or any(c in part for c in '<>"|?*'):
                raise ValueError('Unsafe Windows filename in update archive.')
        key = '/'.join(parts[1:]).casefold()
        if key in seen:
            raise ValueError('Duplicate path in update archive.')
        seen.add(key)
        roots.add(parts[1])
        if not info.is_dir():
            files.append((info, Path(*parts[1:])))
    required = {'lasrecomp.exe', 'lasrecomp.dll.a', 'lasgen/lasgen.exe', 'rando-gen/rando-gen.exe',
                'lasupdate/lasupdate.exe', 'zig/zig.exe', 'gamelib-src/cpu.h'}
    if not required.issubset(seen):
        raise ValueError('Update archive is missing application files.')
    return sorted(roots), files


def remove_app_entry(path):
    # Only called for the allowlisted top-level entries recorded in our transaction.
    if path.is_symlink() or linked(path):
        raise ValueError('Refusing to remove a linked application entry.')
    if path.is_dir():
        for child in path.rglob('*'):
            if linked(child):
                raise ValueError('Refusing to remove a directory containing a link.')
        shutil.rmtree(path)
    else:
        path.unlink()


def rollback(work):
    plan = json.loads((work / 'journal.json').read_text(encoding='utf-8'))
    target = work.parent
    for entry in reversed(plan):
        name = entry['name']
        if not allowed_root(name) or Path(name).name != name:
            raise ValueError('Invalid update recovery journal.')
        dest, old, staged = target / name, work / 'backup' / name, work / 'stage' / name
        safe_path(dest)
        if old.exists():
            if dest.exists():
                remove_app_entry(dest)
            os.replace(old, dest)
        elif not entry['had_old'] and not staged.exists() and dest.exists():
            remove_app_entry(dest)
    (work / 'journal.json').unlink()


def recover(target):
    safe_path(target)
    for work in target.glob('.lasrecomp-update-*'):
        if linked(work):
            raise ValueError('Linked update recovery directory.')
        if (work / 'journal.json').exists() and not (work / 'complete').exists():
            rollback(work)


def install(state, target, current='beta_6', preview='beta_6'):
    safe_path(target)
    if not (target / 'lasrecomp.exe').is_file():
        raise ValueError('This is not a packaged LAS Recomp installation.')
    recover(target)
    offer, package = newer_offer(state, current, preview), state / 'package.zip'
    if package.stat().st_size != offer['size'] or sha256(package) != offer['sha256']:
        raise ValueError('The downloaded update changed. Download it again.')
    work = target / ('.lasrecomp-update-' + uuid.uuid4().hex)
    work.mkdir()
    stage, backup = work / 'stage', work / 'backup'
    stage.mkdir()
    backup.mkdir()
    with zipfile.ZipFile(package) as archive:
        roots, files = package_entries(archive)
        for name in roots:
            if name in APP_DIRS:
                (stage / name).mkdir()
        for info, relative in files:
            dest = stage / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as src, dest.open('xb') as out:
                shutil.copyfileobj(src, out)
        with (stage / 'lasrecomp.exe').open('rb') as exe:
            if exe.read(2) != b'MZ':
                raise ValueError('Update does not contain a Windows executable.')
    plan = []
    for name in roots:
        safe_path(target / name)
        plan.append(dict(name=name, had_old=(target / name).exists()))
    atomic_json(work / 'journal.json', plan)  # write intent before moving even one live file
    try:
        for entry in plan:
            name = entry['name']
            if entry['had_old']:
                os.replace(target / name, backup / name)
            os.replace(stage / name, target / name)
        # The marker is the commit point; leave the old binaries for manual recovery.
        with (work / 'complete').open('x') as f:
            f.write(offer['tag'])
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        rollback(work)
        raise
    status(state, 'installed', 'Update installed. The previous application files were kept in the update backup.', offer['tag'], 1)


def wait_parent(pid):
    if os.name != 'nt':
        raise ValueError('Automatic installation is currently supported only on Windows.')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE, no rights to change the process
    if not handle:
        if ctypes.get_last_error() == 87:  # already exited
            return
        raise OSError('Could not wait for LAS Recomp to close.')
    try:
        if kernel.WaitForSingleObject(handle, 120000) != 0:
            raise TimeoutError('LAS Recomp did not close. The installation was not changed.')
    finally:
        kernel.CloseHandle(handle)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['check', 'download', 'install', 'recover'])
    p.add_argument('--state', type=Path, required=True)
    p.add_argument('--target', type=Path)
    p.add_argument('--data-root', type=Path)
    p.add_argument('--current', default='beta_6')
    p.add_argument('--preview', default='beta_6')
    p.add_argument('--parent', type=int)
    args = p.parse_args()
    state = args.state.absolute()
    try:
        with lock(state):
            if args.action == 'check':
                check(state, args.current, args.preview)
            elif args.action == 'download':
                download(state, args.current, args.preview)
            else:
                if args.parent:
                    status(state, 'waiting', 'Waiting for LAS Recomp to close...')
                    wait_parent(args.parent)
                target = args.target.absolute()
                if args.action == 'recover':
                    recover(target)
                    status(state, 'recovered', 'Previous application files restored.')
                else:
                    install(state, target, args.current, args.preview)
                    # A clean launcher boot, with the same data root, starts any required game-code rebuild.
                    env = dict(os.environ)
                    env['LARECOMP_DATA_ROOT'] = str((args.data_root or state.parent.parent).absolute())
                    subprocess.Popen([str(target / 'lasrecomp.exe')], cwd=target,
                                     env=env,
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except Exception as e:
        try:
            status(state, 'error', str(e))
        except OSError:
            pass
        except ValueError:
            pass
        if args.action in ('install', 'recover') and os.name == 'nt':
            ctypes.windll.user32.MessageBoxW(None, 'LAS Recomp update failed:\n\n' + str(e) +
                '\n\nSee the Updates page or cache/updater/update.log. Your saves were not changed.',
                'LAS Recomp', 0x10)
        print(f'[updater] {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

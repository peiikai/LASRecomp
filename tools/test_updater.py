"""Asset-free tests of the production Windows updater, including interrupted swaps."""
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import updater as u


def offer(tag='v0.6.1', payload=b''):
    name = f'LAS.Recomp-{tag}-windows.zip'
    return dict(tag=tag, name=name, url=u.RELEASES + 'download/' + tag + '/' + name,
                size=len(payload) or 1, sha256=hashlib.sha256(payload).hexdigest())


def release(tag='v0.6.1'):
    o = offer(tag)
    return dict(tag_name=tag, draft=False, assets=[dict(name=o['name'], size=o['size'],
                digest='sha256:' + o['sha256'], browser_download_url=o['url'])])


def package(extra=None):
    files = {'lasrecomp.exe': b'MZ new app', 'lasrecomp.dll.a': b'new imports',
             'lasgen/lasgen.exe': b'new generator', 'rando-gen/rando-gen.exe': b'new randomizer',
             'lasupdate/lasupdate.exe': b'new updater', 'zig/zig.exe': b'new compiler',
             'gamelib-src/cpu.h': b'new header', 'SDL3.dll': b'new SDL'}
    files.update(extra or {})
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as z:
        for name, data in files.items():
            info = zipfile.ZipInfo('placeholder')
            info.filename = info.orig_filename = 'LAS Recomp/' + name  # preserve hostile backslashes on Windows
            z.writestr(info, data)
    return stream.getvalue()


class ReleaseTests(unittest.TestCase):
    def test_version_policy(self):
        tags = ['beta_5', 'beta_5.3', 'v0.5.4', 'beta_6', 'v0.6.1', 'v1.0.0', 'v1.0.1']
        self.assertEqual(sorted(tags, key=u.tag_rank), tags)
        self.assertEqual(u.tag_rank('beta_6'), u.tag_rank('v0.6.0'))
        for tag in ['alpha_1', 'test-foo', 'beta_6.0', 'v0.0.1', 'v1.2.4096', 'v-1.2.3', 'v1.2.3/evil']:
            with self.subTest(tag=tag):
                self.assertEqual(u.tag_rank(tag), -1)

    def test_choose_newest_and_skip_tests_drafts(self):
        draft = release('v3.0.0'); draft['draft'] = True
        selected = u.select_offer([release('v0.6.1'), release('test-foo'), draft,
                                  release('beta_5.4'), release('v0.6.2')], 'beta_6', 'beta_6')
        self.assertEqual(selected['tag'], 'v0.6.2')
        self.assertIsNone(u.select_offer([release('v0.6.0')], 'beta_6', 'beta_6'))
        self.assertEqual(u.select_offer([release('v0.6.0')], 'test-fix', 'beta_6')['tag'], 'v0.6.0')

    def test_untrusted_release_fields(self):
        for key, value in [('url', 'http://evil/evil.zip'), ('url', u.RELEASES + 'download/other/x.zip'),
                           ('sha256', ''), ('sha256', '0' * 63), ('size', -1), ('size', True),
                           ('size', u.MAX_DOWNLOAD + 1), ('name', '../evil.zip')]:
            with self.subTest(key=key, value=value):
                o = offer(); o[key] = value
                with self.assertRaises(ValueError): u.validate_offer(o)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / 'data' / 'cache' / 'updater'; self.state.mkdir(parents=True)
        self.target = self.root / 'App with spaces Ü'; self.target.mkdir()
        (self.target / 'lasrecomp.exe').write_bytes(b'MZ old app')
        (self.target / 'SDL3.dll').write_bytes(b'old SDL')
        for name in ['save/game.sav', 'fonts/custom.ttf', 'rando/seed/file', 'extracted/romfs/file',
                     'cache/shaders/file', 'larecomp.ini', 'lasgame.dll', '.gamebuild/receipt']:
            f = self.target / name; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(b'player data')
        self.original = self.snapshot()
        self.payload = package()
        self.set_package(self.payload)

    def snapshot(self):
        return {str(p.relative_to(self.target)): p.read_bytes() for p in self.target.rglob('*')
                if p.is_file() and not p.relative_to(self.target).parts[0].startswith('.lasrecomp-update-')}

    def set_package(self, payload):
        (self.state / 'package.zip').write_bytes(payload)
        u.atomic_json(self.state / 'offer.json', offer(payload=payload))

    def test_install_preserves_player_files_and_keeps_backup(self):
        u.install(self.state, self.target)
        self.assertEqual((self.target / 'lasrecomp.exe').read_bytes(), b'MZ new app')
        for name, data in self.original.items():
            if name not in ('lasrecomp.exe', 'SDL3.dll'):
                self.assertEqual((self.target / name).read_bytes(), data)
        work, = self.target.glob('.lasrecomp-update-*')
        self.assertEqual((work / 'backup' / 'lasrecomp.exe').read_bytes(), b'MZ old app')
        u.recover(self.target)  # committed installation is kept
        self.assertEqual((self.target / 'lasrecomp.exe').read_bytes(), b'MZ new app')

    def test_every_failed_live_rename_rolls_back(self):
        real_replace = os.replace
        # Two old roots require a backup move; the rest were absent: ten live renames.
        for fail_at in range(1, 11):
            with self.subTest(fail_at=fail_at):
                count = 0
                def replace(src, dst):
                    nonlocal count
                    if Path(src).parent == self.target or Path(src).parent.name == 'stage':
                        count += 1
                        if count == fail_at: raise OSError('simulated locked file')
                    return real_replace(src, dst)
                with patch.object(u.os, 'replace', replace):
                    with self.assertRaises(OSError): u.install(self.state, self.target)
                self.assertEqual(self.snapshot(), self.original)

    def test_every_interrupted_live_rename_recovers_idempotently(self):
        real_replace, real_rollback = os.replace, u.rollback
        class PowerLoss(BaseException): pass
        for fail_at in range(1, 11):
            with self.subTest(fail_at=fail_at):
                count = 0
                def replace(src, dst):
                    nonlocal count
                    result = real_replace(src, dst)
                    if Path(src).parent == self.target or Path(src).parent.name == 'stage':
                        count += 1
                        if count == fail_at: raise PowerLoss()
                    return result
                with patch.object(u.os, 'replace', replace), patch.object(u, 'rollback', lambda work: None):
                    with self.assertRaises(PowerLoss): u.install(self.state, self.target)
                u.recover(self.target)
                u.recover(self.target)
                self.assertEqual(self.snapshot(), self.original)

    def test_corrupt_download_cannot_install(self):
        (self.state / 'package.zip').write_bytes(self.payload + b'bad')
        with self.assertRaises(ValueError): u.install(self.state, self.target)
        self.assertEqual(self.snapshot(), self.original)

    def test_cached_download_cannot_downgrade_a_newer_install(self):
        for current in ['v0.6.1', 'v0.7.0', 'v1.0.0']:
            with self.subTest(current=current):
                with self.assertRaises(ValueError): u.install(self.state, self.target, current=current)
                with self.assertRaises(ValueError): u.download(self.state, current=current)
                self.assertEqual(self.snapshot(), self.original)

    def test_archive_cannot_touch_user_data_or_escape(self):
        for name in ['../outside', 'save/game.sav', 'fonts/custom.ttf', 'larecomp.ini', 'extracted/romfs/file',
                     'lasgame.dll', 'lasgame.dll/evil', 'zig/../../outside', 'zig\\evil', 'zig/NUL', 'zig/file:stream',
                     'zig/file.', 'zig/a/../b', '/absolute', 'cache/updater/worker.exe']:
            with self.subTest(name=name):
                self.set_package(package({name: b'bad'}))
                with self.assertRaises(ValueError): u.install(self.state, self.target)
                self.assertEqual(self.snapshot(), self.original)

    def test_duplicate_and_symlink_entries(self):
        for symlink in [False, True]:
            data = io.BytesIO(self.payload)
            with zipfile.ZipFile(data, 'a') as z:
                info = zipfile.ZipInfo('LAS Recomp/zig/link' if symlink else 'LAS Recomp/SDL3.DLL')
                if symlink: info.external_attr = 0o120777 << 16
                z.writestr(info, b'outside')
            self.set_package(data.getvalue())
            with self.assertRaises(ValueError): u.install(self.state, self.target)

    def test_download_verifies_size_hash_archive(self):
        with patch.object(u, 'request', return_value=io.BytesIO(self.payload)):
            u.download(self.state)
        self.assertTrue((self.state / 'result.txt').read_text().startswith('ready\n'))
        for content in [self.payload[:-1], self.payload + b'x', b'x' * len(self.payload)]:
            with self.subTest(size=len(content)):
                with patch.object(u, 'request', return_value=io.BytesIO(content)):
                    with self.assertRaises(ValueError): u.download(self.state)
                self.assertFalse((self.state / 'package.part').exists())
                self.assertEqual(self.snapshot(), self.original)

    def test_junction_or_link_target_is_refused(self):
        link = self.root / 'redirect'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(self.target), str(link))
        else:
            link.symlink_to(self.target, target_is_directory=True)
        with self.assertRaises(ValueError): u.install(self.state, link)
        self.assertEqual(self.snapshot(), self.original)

    def test_app_directory_junction_is_refused(self):
        link = self.target / 'zig'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(self.state), str(link))
        else:
            link.symlink_to(self.state, target_is_directory=True)
        with self.assertRaises(ValueError): u.install(self.state, self.target)
        self.assertEqual((self.target/'lasrecomp.exe').read_bytes(), b'MZ old app')

    def test_cache_junction_is_refused(self):
        link = self.root / 'redirect-cache'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(self.state), str(link))
        else:
            link.symlink_to(self.state, target_is_directory=True)
        with self.assertRaises(ValueError):
            with u.lock(link): self.fail('cache junction accepted')

    def test_exclusive_worker_lock(self):
        with u.lock(self.state):
            with self.assertRaises(OSError):
                with u.lock(self.state): self.fail('two simultaneous workers')


if __name__ == '__main__':
    unittest.main(verbosity=2)

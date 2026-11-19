#!/usr/bin/env python3
"""The seed's decisions and, for the parity checks, every game file the generator writes (docs/randomizer.md, stage 3).

decisions(out_dir, placements, mods): writes <seed>/decisions.json - what the runtime applies a seed from, without
redoing any of the generator's random draws: the placements (check -> item), item indexes, the settings as the solver
left them, and each random choice made while the files are written (trap models so far; more as stages port them).
`lasrando convert` puts it in seed.json's "decisions".

install(out_dir): with LARECOMP_RECORD=<folder>, every file written under <out_dir>/romfs or <out_dir>/exefs is also
saved there in order (<n>.bin) with write_log.json: [{"n", "path", "sha256", "step", "idgen"}] (step: the module and
function that wrote it; idgen: the event-name counter after the write). tools/rando/test_apply.py replays a seed natively and compares its writes with this log, write by
write, so each ported step is checked even when later steps edit the same file again.
"""
import builtins
import hashlib
import json
import os
import sys
import traceback

_log = []
_dir = None
_out = None
_real_open = builtins.open


_HELPERS = {('mod_generator', 'writeFile'), ('event_tools', 'writeFlow'), ('oead_tools', 'writeSheet')}


def _step():
    """The generator code that is writing: the innermost frame outside this file, the libraries and the write
    helpers."""
    for fr in reversed(traceback.extract_stack()[:-3]):
        f = fr.filename.replace('\\', '/')
        if f.endswith('/record.py') or '/site-packages/' in f or '/lib/python' in f:
            continue
        mod = os.path.basename(f)[:-3]
        if (mod, fr.name) in _HELPERS:
            continue
        return f'{mod}.{fr.name}'
    return '?'


def _idgen():
    """The event-name counter (evfl names each added event AutoEvent<n>; one counter for the whole seed)."""
    m = sys.modules.get('RandomizerCore.Tools.event_tools')
    return m.idgen._id if m is not None else 0


class _Tee:
    def __init__(self, f, rel):
        self._f, self._rel, self._done = f, rel, False

    def write(self, b):
        return self._f.write(b)

    def close(self):
        if self._done:
            return None
        self._done = True
        r = self._f.close()
        # (the bytes as written to disk, not the write() calls joined: writers seek back to fill in offsets - evfl)
        with _real_open(self._f.name, 'rb') as f:
            data = f.read()
        n = len(_log)
        with _real_open(os.path.join(_dir, f'{n}.bin'), 'wb') as o:
            o.write(data)
        _log.append({'n': n, 'path': self._rel, 'sha256': hashlib.sha256(data).hexdigest(), 'step': _step(),
                     'idgen': _idgen()})
        return r

    def __del__(self):  # (open(p, 'wb').write(...) without a close: dungeon_gen, rooms, environments)
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def __getattr__(self, k):
        return getattr(self._f, k)


def _open(file, mode='r', *a, **k):
    f = _real_open(file, mode, *a, **k)
    if isinstance(file, (str, bytes, os.PathLike)) and any(m in mode for m in 'wax'):
        p = os.path.abspath(os.fsdecode(file))
        for top in ('romfs', 'exefs'):
            base = os.path.join(_out, top) + os.sep
            if p.startswith(base):
                return _Tee(f, top + '/' + p[len(base):].replace(os.sep, '/'))
    return f


GROUPS = ('las_items', 'las_world', 'ours_items', 'ours_world')  # (each ported group's own decisions: record_<g>.py)


def _groups():
    import importlib
    return [importlib.import_module('record_' + g) for g in GROUPS]


def install(out_dir):
    global _dir, _out
    for g in _groups():  # (the decisions' hooks: always)
        g.install()
    d = os.environ.get('LARECOMP_RECORD')
    if not d:
        return False
    _dir, _out = os.path.abspath(d), os.path.abspath(out_dir)
    os.makedirs(_dir, exist_ok=True)
    builtins.open = _open
    import atexit
    atexit.register(finish)
    return True


def finish():
    if _dir:
        with _real_open(os.path.join(_dir, 'write_log.json'), 'w') as f:
            json.dump(_log, f, indent=0)


def _plain(v):
    if isinstance(v, (set, frozenset)):
        return sorted(_plain(x) for x in v)
    if isinstance(v, (list, tuple)):
        return [_plain(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _plain(x) for k, x in v.items()}
    return v


TRAP_PICKS = {}  # check -> [model path, model name]: each trap's look, in the order they were drawn


def hook_mods(mods_cls):
    """Record each trap's model pick (ModsProcess.getItemInfo draws it from the shared random stream)."""
    real = mods_cls.getItemInfo

    def getItemInfo(self, check, trap_models=None):
        r = real(self, check, trap_models)
        if trap_models is not None and self.item_defs[self.placements[check]]['item-key'][-4:] == 'Trap':
            TRAP_PICKS.setdefault(check, []).append([r[2], r[3]])
        return r
    mods_cls.getItemInfo = getItemInfo


def decisions(out_dir, placements, settings, item_defs=None, mods=None):
    d = {'version': 1,
         'placements': {k: v for k, v in placements.items() if isinstance(v, str)},
         'indexes': _plain(placements.get('indexes', {})),
         'starting-items': _plain(placements.get('starting-items', [])),
         'dungeon-entrances': _plain(placements.get('dungeon-entrances', {})),
         'settings': _plain(settings),
         'trap-models': TRAP_PICKS,
         # (the items' game names and models as this seed has them: LAS's table with the generator's changes)
         'item-defs': {k: {f: v[f] for f in ('item-key', 'model-path', 'model-name') if f in v}
                       for k, v in (item_defs or {}).items()}}
    for g in _groups():
        g.add(d, mods)
    with _real_open(os.path.join(out_dir, 'decisions.json'), 'w') as f:
        json.dump(d, f, indent=1, sort_keys=True)

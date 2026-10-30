# One-file helper: copied to the data cache before installing, so its own shipped
# directory can be replaced after the game exits. Uses only Python's standard library.
import os
a = Analysis([os.path.join(SPECPATH, 'updater.py')], pathex=[], binaries=[], datas=[],
             hiddenimports=[], excludes=['tkinter', 'unittest', 'pydoc', 'numpy'], noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, a.binaries, a.datas, [], name='lasupdate', console=True, upx=False)

# Updating LAS Recomp on Windows

Open **Settings > Video > Updates** on the start screen, or **Esc / F1 > Video > Updates** while playing.

1. Choose **Check for updates**. LAS Recomp checks the official project's published GitHub releases.
2. If a newer Windows release is available, choose **Download update**. The game can keep running while it downloads.
3. Save in the game, then choose **Install update... > Install and restart**. LAS Recomp closes before the installer
   replaces its files, and starts again afterwards. Unsaved game progress is lost when it closes.

The updater verifies the published size and SHA-256 before offering installation, then verifies them again before
installing. Numbered betas and fixes follow the same ordering as the start screen's update check; test builds are
never offered. A test build can update to the numbered release it previews. A failed/offline check can be retried;
**Release page** always opens the official downloads. Checks and downloads can be cancelled.

Only the Windows application package is replaced. Saves, seeds, imported fonts, settings, extracted game files,
caches and the locally built `lasgame.dll` stay in place, including when you use a separate data folder. The next
start performs the usual game-code rebuild if the new release needs one. No game update or game content is downloaded.

This first version supports the official portable Windows ZIP. macOS and Linux keep their existing start-screen
release links. Developer builds need `make update-helper` for the dialog; install testing requires an isolated
packaged layout. A release without the new helper, or without GitHub's package digest, cannot be installed this way.
Use the release page to update such a release manually. No administrator elevation is requested: the installation
must be writable. Paths through symlinks or junctions, including the update cache, are refused. A process job imposed
by another launcher may prevent the installer from outliving LAS Recomp; use the release page if handoff fails.

## Failed or interrupted installations

A normal file-replacement failure rolls back the files already replaced. The installer reports the error and does
not restart the game. A successful update keeps the previous application files under
`.lasrecomp-update-<id>/backup/` in the application folder. These backups are not automatically deleted.

An interruption such as power loss can leave an incomplete transaction. The next installation attempt recovers it
before starting a new one. If LAS Recomp will not launch, close any remaining LAS Recomp processes and run the cached
helper from PowerShell (replace both example paths with your installation and data folders):

```powershell
& 'D:\LAS data\cache\updater\install-worker.exe' recover `
  --state 'D:\LAS data\cache\updater' --target 'D:\Games\LAS Recomp'
```

The recovery command restores only unfinished transactions. It is safe to repeat. It must be run with LAS Recomp
closed. The helper's result is in `cache/updater/result.txt`; diagnostics are in `cache/updater/update.log` in the
data folder. Re-extracting the official ZIP over the application is also a manual recovery option; keep your data.

## Building and testing

`make update-helper` freezes `tools/updater.py` with the existing helper-build Python environment. `make dist`
requires the result and includes `lasupdate/lasupdate.exe`. The helper has no third-party Python runtime dependencies.

`python tools/test_updater.py` uses synthetic releases and archives, tests size/hash failures and invalid paths,
checks player-file preservation, and injects failures/interruption at every live rename followed by rollback/recovery.
It does not contact GitHub or install anything into a real application.

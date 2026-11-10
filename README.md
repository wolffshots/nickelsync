# NickelSync

NickelSync adds a **Sync now** item to NickelMenu. The item connects to Wi-Fi and starts a native Kobo sync. It works inside a book, so you do not have to go to the home screen.

The sync is the same sync as the stock **Sync** button. It uses the sync server in `Kobo eReader.conf`, so it also works with Calibre-Web, Autocaliweb and Calibre-Web-NextGen.

Status: experimental. `N3SyncManager::silentSync()` is present on a Libra Colour. No test on a device is complete yet.

## Requirements

- [NickelMenu](https://github.com/pgaskin/NickelMenu). NickelSync has no menu of its own.
- A Kobo firmware that exports `N3SyncManager::silentSync()`. If a required symbol is missing, NickelHook does not load the mod. Nickel keeps working.

## Install

With [kpm](https://github.com/wolffshots/kpm):

```sh
kpm install nickelsync
```

Without kpm:

1. Download `KoboRoot.tgz` from the [latest release](https://github.com/wolffshots/nickelsync/releases/latest).
2. Copy it to `KOBOeReader/.kobo/` on the device.
3. Eject the device. Nickel installs the file and reboots.

## Use

Open the NickelMenu menu in a book, or the main NickelMenu menu, and tap **Sync now**.

NickelSync shows a short message for each step:

| Message | Meaning |
|---|---|
| Connecting to Wi-Fi | Wi-Fi was not connected. NickelSync connects it. |
| Syncing | NickelSync asked Nickel to sync. |
| Sync done | The sync finished, and Nickel updated its last sync time. |
| Sync not confirmed | The sync finished, but Nickel did not update its last sync time. The sync can have failed. Check the result on the server. |
| Sync did not start | Nickel did not start a sync within 20 seconds. |
| Sync timed out | The sync did not finish within 5 minutes. |
| Could not connect to Wi-Fi | The Wi-Fi connection failed. |
| Wi-Fi connection timed out | The Wi-Fi connection took longer than `ConnectTimeout`. |
| Sync already running | A sync is in progress. NickelSync does not start a second one. |

The menu item writes to `/tmp/nickelsync`. NickelSync watches this file. You can also start a sync from a script with `echo > /tmp/nickelsync`.

## Configuration

The configuration file is `KOBOeReader/.adds/nickelsync/config.ini`. The file is optional. NickelSync reads it each time you tap **Sync now**, so a change has effect without a reboot.

In kpm, open NickelSync and tap **Settings** to create and edit the file.

```ini
[NickelSync]
; Seconds to wait for a Wi-Fi connection (10 to 180).
ConnectTimeout=60
; If Wi-Fi was off before the sync, turn it off again after the sync.
RestoreWifi=true
```

`RestoreWifi` does not turn Wi-Fi off if Nickel starts to download books after the sync.

## Log

Each sync writes a log to `KOBOeReader/.adds/nickelsync/log.txt`. The next sync replaces the file. NickelSync also writes to the system log with the prefix `NickelSync:`.

## Uninstall

With kpm:

```sh
kpm uninstall nickelsync
```

Without kpm:

1. Delete `KOBOeReader/.adds/nickelsync/uninstall`.
2. Reboot the device.

NickelHook then removes the mod. The mod also removes its NickelMenu file, its configuration and its log.

## How it works

NickelSync is a [NickelHook](https://github.com/pgaskin/NickelHook) mod. When the trigger file changes, it does these steps:

1. If Wi-Fi is not connected, it calls `WirelessWorkflowManager::connectWirelessSilently()`. It then waits for the `networkConnected()` signal.
2. It calls `N3SyncManager::silentSync()`.
3. It reads the `syncStarted`, `finished` and `downloadStarted` signals of the sync manager.
4. After `finished`, it compares `ApplicationPreferences/LastSyncTime` in `Kobo eReader.conf` with the value before the sync.

[Kobo AutoSync](https://github.com/kroshka4aplin/kobo-autosync) showed the `silentSync()` call and this result model.

## Safety

NickelSync is designed so that it cannot stop the device from booting:

- **No hooks.** NickelSync replaces no Nickel function. Before the first tap on **Sync now**, it only watches `/tmp/nickelsync`.
- **Missing symbols.** If the firmware does not have a required Nickel function, NickelHook does not load the mod. Nickel starts as usual without it.
- **Crash at load.** If Nickel crashes within 15 seconds after it loads the mod, the NickelHook failsafe keeps the mod disabled on the next boot.
- **Uninstall without the menu.** Connect the device over USB, delete `KOBOeReader/.adds/nickelsync/uninstall` and reboot.

## Build

The build uses [NickelTC](https://github.com/pgaskin/NickelTC) in a container:

```sh
git submodule update --init
podman run --rm -v "$PWD":/src -w /src ghcr.io/pgaskin/nickeltc:1.0 make clean all koboroot
```

The build writes `KoboRoot.tgz`. A `v*` tag runs the release workflow, which builds the file and attaches it to a GitHub release.

## License

MIT. See [LICENSE](LICENSE).

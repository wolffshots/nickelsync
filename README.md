# NickelSync

NickelSync adds a **Sync now** item to NickelMenu. The item connects to Wi-Fi and starts a native Kobo sync. It works inside a book, so you do not have to go to the home screen.

Inside a book, NickelSync also syncs the reading position of the open book. The stock sync does not do that. Nickel sends the position only when you leave the book.

The sync is the same sync as the stock **Sync** button. It uses the sync server in `Kobo eReader.conf`, so it also works with Calibre-Web, Autocaliweb and Calibre-Web-NextGen.

Status: tested on a Kobo Libra Colour with firmware 4.46.23852. The test used Calibre-Web-NextGen, with a second device that syncs the same book.

## Requirements

- [NickelMenu](https://github.com/pgaskin/NickelMenu). NickelSync has no menu of its own.
- A Kobo firmware that exports `N3SyncManager::silentSync()`. If a required symbol is missing, NickelHook does not load the mod. Nickel keeps working.
- For the reading position: a kepub book that Nickel syncs with the server.

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

NickelSync shows a short message for each step. The messages show at the top of the screen, so they do not cover a Nickel prompt in the center:

| Message | Meaning |
|---|---|
| Connecting to Wi-Fi | Wi-Fi was not connected. NickelSync connects it. |
| Syncing | NickelSync started to sync. |
| Sync done | The sync finished, and Nickel updated its last sync time. |
| Sync done, Reading position sent | Inside a book: NickelSync also sent the position of the open book to the server. |
| Sync done, Reading position not synced | Inside a book: NickelSync did not get the position from the server, so it did not send the position. See [Reading position](#reading-position). |
| Sync not confirmed | The sync finished, but Nickel did not update its last sync time. The sync can have failed. Check the result on the server. |
| Sync did not start | Nickel did not start a sync within 20 seconds. |
| Sync timed out | The sync did not finish within 5 minutes. |
| Could not connect to Wi-Fi | The Wi-Fi connection failed. |
| Wi-Fi connection timed out | The Wi-Fi connection took longer than `ConnectTimeout`. |
| Sync already running | A sync is in progress. NickelSync does not start a second one. |

The menu item writes to `/tmp/nickelsync`. NickelSync watches this file. You can also start a sync from a script with `echo > /tmp/nickelsync`.

## Reading position

When you tap **Sync now** inside a book, NickelSync first gets the position of the book from the server. If the server has a newer position, for example from another device, Nickel shows its stock prompt "Sync to last page read?":

- Tap the accept button to go to the server position.
- Tap the cancel button to stay on the current page.

NickelSync does not send the Kobo position while the prompt is open. If no prompt shows, NickelSync sends the current page to the server. Then it runs the full sync.

The order matters. If the Kobo sent its position first, the server would never have a newer position, and the prompt would never show.

Limits:

- The position sync works for kepub books that Nickel syncs with the server. Nickel does not sync the position of a sideloaded book. For such a book, NickelSync waits 20 seconds for the server, then runs the full sync only.
- If the server does not answer, NickelSync does not send the position. This keeps a newer position from another device on the server.
- Nickel itself sends the position when you leave the book or connect USB. So after you cancel the prompt, the Kobo position goes to the server when you leave the book.

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
2. If a kepub book is open, it gets the position from the server. It finds the `ReadingView` with `MainWindowController::viewWithObjectName()` and the reader child that inherits `KepubBookReaderBase`. It invokes the reader slot `onResume()`, which Nickel also calls when the device wakes. Nickel then sends `GET .../state` and shows the prompt if the server is newer.
3. It waits for the `bookmarkRequestSuccess(Volume)` signal of the sync manager, then 1.5 seconds more. If a `ConfirmationDialog` is visible, it does not send the position.
4. If no prompt is visible, it emits the `ReadingView` signals `saveBookmark()` and `sendBookmark(Volume)`. Nickel emits the same two signals when you leave a book. Nickel then sends `PUT .../state`.
5. It calls `N3SyncManager::silentSync()`.
6. It reads the `syncStarted`, `finished` and `downloadStarted` signals of the sync manager.
7. After `finished`, it compares `ApplicationPreferences/LastSyncTime` in `Kobo eReader.conf` with the value before the sync.

Nickel shows each message in an `N3ToastView` window in the center of the screen. While a NickelSync message waits to show, NickelSync installs an application event filter. When an `N3ToastView` shows with a NickelSync text, the filter moves the window to the top. The move comes before Nickel draws the window. Nickel's own messages stay in the center.

[Kobo AutoSync](https://github.com/kroshka4aplin/kobo-autosync) showed the `silentSync()` call and this result model.

## Safety

NickelSync is designed so that it cannot stop the device from booting:

- **No hooks.** NickelSync replaces no Nickel function. Before the first tap on **Sync now**, it only watches `/tmp/nickelsync`.
- **Missing symbols.** If the firmware does not have a required Nickel function, NickelHook does not load the mod. Nickel starts as usual without it. The two functions for the reading position are optional. If a firmware does not have them, NickelSync runs the full sync only.
- **Calls by name.** For the reading position, NickelSync calls Nickel slots and signals by name. If a name does not exist, the call fails without a crash.
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

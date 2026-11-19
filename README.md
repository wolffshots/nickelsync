# NickelSync

NickelSync adds a sync icon to the reading menu of a book. The icon opens the NickelSync page with **Sync now**. **Sync now** connects to Wi-Fi and starts a native Kobo sync. You do not have to go to the home screen. If you use NickelMenu, NickelSync also adds a **Sync now** item to it.

Inside a book, NickelSync also syncs the reading position of the open book. The stock sync does not do that. Nickel sends the position only when you leave the book.

The sync is the same sync as the stock **Sync** button. It uses the sync server in `Kobo eReader.conf`, so it also works with Calibre-Web, Autocaliweb and Calibre-Web-NextGen.

Status: tested on a Kobo Libra Colour with firmware 4.46.23852. The test used Calibre-Web-NextGen, with a second device that syncs the same book.

## Requirements

- Optional: [NickelMenu](https://github.com/pgaskin/NickelMenu), for the **Sync now** item on the home screen.
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

In a book, tap the center of the page to open the reading menu. Tap the sync icon in the top bar, next to the more options icon. The icon opens the NickelSync page. The page shows the time of the last sync and the result of the last NickelSync sync since the last restart. Tap a button to close the page and start the sync:

| Button | Action |
|---|---|
| Sync now | Syncs the reading position, then runs the full sync. The `QuickSync` setting does not apply. |
| Sync reading position | Syncs only the reading position, as with `QuickSync=true`. See [Quick sync](#quick-sync). |

With NickelMenu, you can also tap **Sync now** in the NickelMenu menu of a book or of the home screen. This item uses the `QuickSync` setting.

NickelSync shows a short message for each step. The messages show at the top of the screen, so they do not cover a Nickel prompt in the center:

| Message | Meaning |
|---|---|
| Connecting to Wi-Fi | Wi-Fi was not connected. NickelSync connects it. |
| Syncing | NickelSync started to sync. |
| Sync done | The sync finished, and Nickel updated its last sync time. |
| Sync done, Reading position sent | Inside a book: NickelSync also sent the position of the open book to the server. |
| Sync done, Reading position not synced | Inside a book: NickelSync did not get the position from the server, so it did not send the position. See [Reading position](#reading-position). |
| Reading position sent | `QuickSync` inside a book: NickelSync sent the position of the open book and skipped the full sync. |
| Reading position received | `QuickSync` inside a book: the server has a newer position, and Nickel shows its prompt. NickelSync skipped the full sync. |
| Reading position not synced | `QuickSync` inside a book: NickelSync could not send the position. |
| Sync not confirmed | The sync finished, but Nickel did not update its last sync time. The sync can have failed. Check the result on the server. |
| Sync did not start | Nickel did not start a sync within 20 seconds. |
| Sync timed out | The sync did not finish within 5 minutes. |
| Could not connect to Wi-Fi | The Wi-Fi connection failed. |
| Wi-Fi connection timed out | The Wi-Fi connection took longer than `ConnectTimeout`. |
| Sync already running | A sync is in progress. NickelSync does not start a second one. |

The NickelMenu item writes to `/tmp/nickelsync`. NickelSync watches this file. You can also start a sync from a script with `echo > /tmp/nickelsync`.

## Reading position

When you tap **Sync now** inside a book, NickelSync first gets the position of the book from the server. If the server has a newer position, for example from another device, Nickel shows its stock prompt "Sync to last page read?":

- Tap the accept button to go to the server position.
- Tap the cancel button to stay on the current page.

NickelSync does not send the Kobo position while the prompt is open. If no prompt shows, NickelSync sends the current page to the server. Then it runs the full sync.

The order matters. If the Kobo sent its position first, the server would never have a newer position, and the prompt would never show.

### Quick sync

Set `QuickSync=true` in the [configuration](#configuration) to sync only the reading position inside a book. NickelSync then gets the position and sends it, or Nickel shows its prompt. NickelSync does not run the full sync, so the sync ends sooner.

In these cases NickelSync runs the full sync as usual, also with `QuickSync=true`:

- No book is open, for example on the home screen.
- The server does not send the position within 20 seconds, for example for a sideloaded book.

Limits:

- The position sync works for kepub books that Nickel syncs with the server. Nickel does not sync the position of a sideloaded book. For such a book, NickelSync waits 20 seconds for the server, then runs the full sync only.
- If the server does not answer, NickelSync does not send the position. This keeps a newer position from another device on the server.
- Nickel itself sends the position when you leave the book or connect USB. So after you cancel the prompt, the Kobo position goes to the server when you leave the book.
- With `QuickSync`, NickelSync cannot see when Nickel finishes the upload. If `RestoreWifi` turns Wi-Fi off again, NickelSync first waits 15 seconds. During this wait, **Sync now** shows "Sync already running".

## Configuration

The configuration file is `KOBOeReader/.adds/nickelsync/config.ini`. The file is optional. NickelSync reads it each time you tap **Sync now**, so a change has effect without a reboot.

In kpm, open NickelSync and tap **Settings** to create and edit the file.

```ini
[NickelSync]
; Seconds to wait for a Wi-Fi connection (10 to 180).
ConnectTimeout=60
; If Wi-Fi was off before the sync, turn it off again after the sync.
RestoreWifi=true
; Inside a book, sync only the reading position and skip the full sync.
QuickSync=false
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

NickelSync is a [NickelHook](https://github.com/pgaskin/NickelHook) mod. It installs an application event filter when Nickel loads it. The filter acts only on show events. When a `ReadingMenuView` shows for the first time, the filter finds the `bottomHorizontalLayout` bar and the `settingsIcon`. On firmware 4.46 this bar is the lower row of the reading menu header, at the top of the screen. `settingsIcon` is the gear, and the last icon is the more options icon. The filter puts a `TouchLabel` with the Nickel status bar sync image before the last icon. NickelHardcover adds its icon in the same way, but from a hook. So the mod icons stay together, between the gear and the more options icon. A tap on the icon opens the NickelSync page, an `N3Dialog` that `MainWindowController::pushView()` shows over the book. A tap on a page button closes the page. When Nickel shows the book again, NickelSync starts the sync. If the firmware does not have the `TouchLabel`, `N3ButtonLabel`, `N3Dialog` or `pushView()` functions, the reading menu has no icon.

When you tap a page button or the trigger file changes, NickelSync does these steps:

1. If Wi-Fi is not connected, it calls `WirelessWorkflowManager::connectWirelessSilently()`. It then waits for the `networkConnected()` signal.
2. If a kepub book is open, it gets the position from the server. It finds the `ReadingView` with `MainWindowController::viewWithObjectName()` and the reader child that inherits `KepubBookReaderBase`. It invokes the reader slot `onResume()`, which Nickel also calls when the device wakes. Nickel then sends `GET .../state` and shows the prompt if the server is newer.
3. It waits for the `bookmarkRequestSuccess(Volume)` signal of the sync manager, then 1.5 seconds more. If a `ConfirmationDialog` is visible, it does not send the position.
4. If no prompt is visible, it emits the `ReadingView` signals `saveBookmark()` and `sendBookmark(Volume)`. Nickel emits the same two signals when you leave a book. Nickel then sends `PUT .../state`.
5. It calls `N3SyncManager::silentSync()`. With `QuickSync=true`, it stops before this step if it got the position in step 3.
6. It reads the `syncStarted`, `finished` and `downloadStarted` signals of the sync manager.
7. After `finished`, it compares `ApplicationPreferences/LastSyncTime` in `Kobo eReader.conf` with the value before the sync.

Nickel shows each message in an `N3ToastView` window in the center of the screen. When an `N3ToastView` shows with a NickelSync text, the event filter moves the window to the top. The move comes before Nickel draws the window. Nickel's own messages stay in the center.

[Kobo AutoSync](https://github.com/kroshka4aplin/kobo-autosync) showed the `silentSync()` call and this result model.

## Safety

NickelSync is designed so that it cannot stop the device from booting:

- **No hooks.** NickelSync replaces no Nickel function. Before a reading menu shows, it only watches `/tmp/nickelsync` and checks the type of each Qt event in its event filter.
- **Missing symbols.** If the firmware does not have a required Nickel function, NickelHook does not load the mod. Nickel starts as usual without it. The two functions for the reading position are optional. If a firmware does not have them, NickelSync runs the full sync only. The functions for the icon are also optional. If a firmware does not have them, the reading menu has no icon.
- **Calls by name.** For the reading position, NickelSync calls Nickel slots and signals by name. If a name does not exist, the call fails without a crash.
- **Crash at load.** If Nickel crashes within 15 seconds after it loads the mod, the NickelHook failsafe keeps the mod disabled on the next boot.
- **Uninstall without a menu.** Connect the device over USB, delete `KOBOeReader/.adds/nickelsync/uninstall` and reboot.

## Build

The build uses [NickelTC](https://github.com/pgaskin/NickelTC) in a container:

```sh
git submodule update --init
podman run --rm -v "$PWD":/src -w /src ghcr.io/pgaskin/nickeltc:1.0 make clean all koboroot
```

The build writes `KoboRoot.tgz`. A `v*` tag runs the release workflow, which builds the file and attaches it to a GitHub release.

## License

MIT. See [LICENSE](LICENSE).

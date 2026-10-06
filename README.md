# NasDlc

A DashLaunch plugin for the Xbox 360. It loads DLC and title updates from a NAS share (ConnectX), so that you do not have to copy them to the HDD.

## Function

The plugin patches the kernel file imports of XAM. When XAM searches for DLC or a title update on the HDD, the plugin also searches the NAS:

- **DLC (content type `00000002`):** the NAS packages are added to the HDD packages. If the same package is on the HDD and on the NAS, XAM gets it only once (from the HDD).
- **Title updates (content type `000B0000`):** if the title update file is not on the HDD, XAM opens it on the NAS.
- **Cache:** the plugin keeps the start of each NAS package in memory. This decreases the number of network requests and the load time.

The plugin changes only reads. It does not write to the NAS, and new files (for example new DLC downloads) always go to the HDD.

## Requirements

- Xbox 360 with DashLaunch (tested: dashboard 17559, ABadAvatar + XeUnshackle).
- ConnectX, connected to an SMB share.
- A FAT32 USB drive as the first USB device (`\Device\Mass0`), for `NasDlc.ini` and `NasDlc.log`.

## Installation

1. Copy `NasDlc.xex` and `NasDlc.ini` to the root of the USB drive.
2. Add the plugin to `launch.ini`, for example:
   ```ini
   [Plugins]
   plugin1 = Usb:\NasDlc.xex
   ```
3. Change `NasContent` in `NasDlc.ini` to your server and share name.
4. Start the console. You see the notification `NasDlc v13.4 active`, and later `NAS connected`.

## NAS folder structure

The NAS uses the same structure as the HDD, below the `Content` folder of the share:

```
Content/
  0000000000000000/
    <TitleID>/
      00000002/              DLC packages (original file names)
      000B0000/              Title updates (lowercase tu… files only)
```

Examples:

- Guitar Hero II DLC: `Content/0000000000000000/415607E7/00000002/<package>`
- Minecraft title update: `Content/0000000000000000/584111F7/000B0000/tu00000001_00000000`

Rules:

- Do not change the file names.
- Title updates: only **lowercase** `tu…` files work. Uppercase `TU_…` files belong in the system cache of the HDD (`Hdd1\Cache`). Install them with Aurora.
- On a Mac, remove the `._*` and `.DS_Store` files (`dot_clean -m <folder>`). The plugin skips them, but a clean folder is better.
- The share can be read-only.

## Settings (`NasDlc.ini`)

All settings are optional. A missing setting keeps its default value.

| Section | Setting | Default | Description |
|---|---|---|---|
| `[Paths]` | `HddContent` | `\Device\Harddisk0\Partition1\Content\` | HDD content folder, as XAM sees it. |
| `[Paths]` | `NasContent` | `\Network\Smb\DOCKER\XBOXSMB\Content\` | NAS content folder: `\Network\Smb\<server>\<share>\Content\` |
| `[Content]` | `Dlc` | `1` | Load DLC from the NAS. |
| `[Content]` | `TitleUpdates` | `1` | Load title updates from the NAS. |
| `[Notify]` | `Start` | `1` | Notifications "active" and "NAS connected". |
| `[Notify]` | `Found` | `1` | Notifications "N DLC packages found" and "title update loaded". |
| `[Log]` | `Level` | `1` | `0` = no log, `1` = important lines, `2` = all details. |
| `[Log]` | `TuTrace` | `0` | Diagnostic: log each title update search of XAM. |
| `[Log]` | `ContentTrace` | `0` | Diagnostic: log each XAM access to a `\Content\` path and each listing of a `\Content\` folder, on all devices. |
| `[Cache]` | `Enabled` | `1` | Header cache. Set to `0` only for a problem search. |
| `[Cache]` | `Verify` | `0` | Diagnostic: read from the NAS and compare with the cache (slower). |
| `[Speed]` | `Prefetch` | `0` | Test feature: worker threads fill the cache of each NAS DLC package before XAM opens it. Needs `Cache.Enabled = 1`. |
| `[Speed]` | `PrefetchThreads` | `2` | Number of prefetch worker threads (`1` to `4`). |

Boolean values: `1`/`0`, `true`/`false`, `yes`/`no`, `on`/`off`.

## Notifications

| Notification | Meaning |
|---|---|
| `NasDlc v13.4 active` | The hooks are installed. |
| `NasDlc v13.4: hook count wrong` | The plugin could not install all hooks. It may not work. Send the log. |
| `NAS connected: DLC and title updates ready` | The NAS share is available. |
| `NAS: N DLC packages found` | The NAS gave XAM N packages for this game. |
| `NAS: title update loaded` | XAM opened a title update on the NAS. |

## Log

`NasDlc.log` is in the root of the USB drive. The plugin adds to the file at each boot. Delete the file from time to time.

The first lines show the settings that the plugin uses. If `unknown` is not `0`, a setting in `NasDlc.ini` has a spelling mistake.

## Load time

Each NAS request costs one video frame (16.6 ms) while a game runs. XAM reads DLC headers in many small pieces. The cache decreases this, but DLC from the NAS loads slower than from the HDD. Example: Guitar Hero II with 10 DLC packages loads in about 6 s (without the cache: about 13 s). Title updates load during the game launch, so they cost almost no extra time.

## Limits

- The plugin also redirects profile-specific content (folders with a profile ID instead of `0000000000000000`), if the same folder exists on the NAS.
- Uppercase `TU_…` title updates are not supported.
- Content on USB storage devices is not redirected.
- If the NAS is not connected, the plugin has no effect. The console then uses only the HDD.
- **The title folder must exist on the HDD.** Some games (for example Guitar Hero 5) search for DLC only if `Hdd1\Content\0000000000000000\<TitleID>\00000002\` exists on the HDD. Create this folder (it can be empty) for each game with DLC on the NAS.

## Build

Requirements: Windows, Visual Studio 2010, XDK 21256.3, xkelib.

Project settings:

- Configuration type: Title (`.xex`).
- Linker, additional options: `/DLL /ENTRY:"_DllMainCRTStartup" /ALIGN:128,4096`
- XEX configuration file: `NasDlc.xml` (system DLL, base address `0x91E00000`, compressed).
- Ignored libraries: `kernelext.lib`.
- Precompiled headers: off.
- Deployment: excluded from the build.

Check the output with `xextool -l NasDlc.xex`: load address `91E00000`, page size `1000`, DLL module (not a title module).

If another plugin uses the base address `0x91E00000`, change it in `NasDlc.xml`.

xkelib include folder: set the environment variable `XKELIB_DIR` (or pass `/p:XkelibDir=<path>` to MSBuild). Without it, the project uses the folder `xkelib` next to `NasDlc.sln`.

Command line build:

```bat
%WINDIR%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe NasDlc.sln /p:Configuration=Release "/p:Platform=Xbox 360"
```

## Technical notes

- All hooks pass every parameter as a raw 64-bit value. A type conversion breaks `NtQueryDirectoryFile` (the Xbox 360 version has no `FileInformationClass` parameter).
- The NAS refuses read/write opens. XAM then retries read-only. The plugin gives the "access denied" status to XAM for this reason.
- Reads without an offset (from the current file position) always go to the NAS, because XAM sets this position itself.

## Findings for later work (kernel analysis)

Diagnostic versions 14a to 14d examined the kernel side of the DLC load. They are not part of this release. These are the results:

**SMB filter driver**

- The `\Network\Smb` device belongs to a filter driver in a module at about `0x91E80000`. The filter renames the real device to `\Network\RealSmb` and puts itself in front of it.
- This module is not one of the `launch.ini` plugins (`xbdm.xex` is at `0x91000000`, `JRPC2.xex` at `0x91900000`). Probably ConnectX or the exploit loader installs it.
- **Address limit:** NasDlc loads at `0x91E00000` with a size of `0x3C000`. The plugin must stay below `0x91E80000`, or it overlaps with this module.

**Kernel object layout (Xbox 360, from memory dumps)**

| Object | Offset | Content |
|---|---|---|
| File object | `+04` | Device object |
| Device object | `+08` | Driver object |
| Driver object | `+0C` | `MajorFunction[]`, 11 entries (index 2 = read) |
| IRP | `+00` | Type `6`, size `0x84` |
| IRP | `+1C` | Buffer (for all reads, also kernel reads) |
| IRP | `+50` | Current stack location (`IRP + 0x60`) |
| IRP | `+54` | File object |
| Stack location | `+60` | Major function |
| Stack location | `+64` | Length |
| Stack location | `+6C` | Offset (64 bits, only 4-byte aligned) |
| Stack location | `+78` | File object |

A read hook works without code patches: copy the driver object, change the read entry in the copy, and set `device + 8` to the copy. Note: the system reuses file object addresses after a handle closes.

**Read pattern and delay**

- Per package, the kernel STFS mount reads `0x344` (4 bytes), `0x971A` (the XAM read without offset), and the 4 KB blocks `B6000`, `B000`, `C000` and `D000`.
- During the game, each NAS request from a XAM thread takes about 16.5 ms (one frame), and large reads cost about one frame per 8 KB. The same reads from a game thread take about 0.6 ms (64 KB in about 7 ms).
- Without a running game (for example during a title update load at launch), all requests are fast.
- A higher thread priority does not change the delay (test v14d: 28.14 s with boost, 28.02 s without). The cause is probably inside the SMB driver.

**Not implemented**

- A read-ahead cache at the driver level could save about 3.5 frames per package (about 0.6 s for 10 packages). The plugin would have to complete kernel requests itself, so the risk was too high for this gain.
- Parallel prefetch of all package caches during the listing: version 13.3 adds it as a test feature (`Speed.Prefetch`, off by default). Test results are not available yet.

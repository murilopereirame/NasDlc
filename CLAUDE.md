# CLAUDE.md — NasDlc

Context for Claude Code agents. Read this file and `FOLLOWUP.md` before you change code.

## Working with the user

- The user is Rodrigo. He writes the code on a Windows VM (Visual Studio 2010) and tests on a real Xbox 360.
- **Write in ASD-STE100 Simplified Technical English. Use short, concise sentences.**
- You cannot run or test the code. Each test is a manual build, a copy to a USB drive, and a console boot. Thus:
  - Make small, safe changes. Put new or risky behavior behind an INI setting, off by default.
  - Add log lines that prove or disprove your theory in **one** test.
  - Ask for the log (`NasDlc.log`) after each test, and read it carefully before the next change.
- Before you write code for an unknown system behavior, write a diagnostic (trace) version first.

## Project

NasDlc is a DashLaunch plugin (system DLL, `.xex`) for the Xbox 360. It loads DLC (content type `00000002`) and title updates (`000B0000`) from a NAS share, so that they do not need space on the HDD.

Current release: **version 13.2**. Version 13.3 (in test) adds the parallel prefetch (`Speed.Prefetch`, off by default). Version 13.4 (in test) adds the listing trace (`CT list` lines) and the title folder fix (`Content.TitleFolders`, off by default) (`NasDlc/nasdlc.cpp`). It works with Guitar Hero II (`415607E7`), Guitar Hero 5 (`41560840`) and Minecraft (`584111F7`).

## Environment

| Item | Value |
|---|---|
| Console | Xbox 360, dashboard 17559, **not RGH** |
| Exploit | ABadAvatar → XeUnshackle → DashLaunch |
| HDD | Third-party HDD (ABadStorage). `\Device\Harddisk0\Partition1` |
| USB | FAT32, first USB device (`\Device\Mass0`). Holds `launch.ini`, the plugins, `NasDlc.ini`, `NasDlc.log`. Also configured as a storage device (`\Device\Mass0PartitionFile\Storage`) |
| NAS | SMB share through ConnectX: `\Network\Smb\DOCKER\XBOXSMB\Content\`. Read-only |
| Launcher | Aurora (title ID `FFFE07DF`). Games run from the NAS |
| Other plugins | `xbdm.xex` (`0x91000000`), `JRPC2.xex` (`0x91900000`) |
| Dev | Windows VM, Visual Studio 2010, XDK 21256.3, xkelib. The user also uses a Mac (run `dot_clean -m` on the USB drive before eject) |

## Build

- Configuration type: Title (`.xex`). Linker options: `/DLL /ENTRY:"_DllMainCRTStartup" /ALIGN:128,4096`.
- XEX config: `NasDlc.xml` (`<sysdll/>`, base `0x91E00000`, compressed).
- Ignored library: `kernelext.lib`. No precompiled headers. Deployment excluded.
- Check with `xextool -l`: load address `91E00000`, DLL module.
- Solution: `NasDlc.sln`, project `NasDlc/NasDlc.vcxproj`. On the user's VM, the project file may still be named `NasDlcTrace`. The `.cpp` in the project must contain the new code. **Always select Rebuild.** Check the version in the first log line (`==== NasDlc v13.2 loaded`).
- **Address limit:** the plugin image is `0x3C000` bytes. An unknown module (the SMB filter driver) is at `0x91E80000`. The plugin must stay below it.

## Architecture (version 13.2)

The plugin patches the **kernel imports of XAM** (import table slot + call stub). Hooks: `NtOpenFile`, `NtCreateFile`, `NtQueryDirectoryFile`, `NtQueryFullAttributesFile`, `NtReadFile`, `NtClose`.

1. **Path mapping.** `HddContent + <16 hex>\<8 hex>\<type>[\...]` → `NasContent + same rest`. Only types `00000002` and `000B0000`, if enabled.
2. **Folder open.** HDD folder exists → also open the NAS folder (merge). HDD folder missing → XAM gets the NAS folder.
3. **Listing.** After the last HDD entry (`80000006`), the listing continues with the NAS entries. Duplicates (same name on the HDD) and junk (`.`, `..`, `._*`, `.DS_Store`) are skipped. A non-NULL mask restarts the scan.
4. **File open.** HDD "not found" → open on the NAS. `NtCreateFile` only for `FILE_OPEN` (disposition 1): never redirect a create.
5. **Read-only NAS.** XAM opens read/write first. The NAS refuses (`C0000022`). The plugin passes this status to XAM, and **XAM retries read-only**. After the first refusal, known NAS files (seen in a NAS listing or opened) are refused at once.
6. **Header cache.** Per NAS path, the first bytes of the package stay in memory: fill `0-1000`, then up to the highest offset seen in this title (`B000` for Guitar Hero), max `10000`. Idle entries are freed after 20 s and at each title change.
7. **Notifications** (`XNotifyQueueUI`) are sent only from the watch thread, never from a hook.
8. **Log.** Hooks only copy text into a memory buffer. The watch thread writes the file every 0.5 s, under `g_FileLock`.
9. **Settings.** `UsbX:\NasDlc.ini` (see `README.md`).
10. **Prefetch (v13.3, `Speed.Prefetch = 0` by default).** When XAM opens a NAS DLC folder, a worker thread lists the same folder with its own handle and queues each package. 1 to 4 worker threads (system threads) open each package read-only and fill `0-1000` of its cache entry. A worker stays max. 8 packages in front of XAM. The hooks only queue a folder; they never wait for a worker, except after a sharing violation on a package that a worker has open (max. 500 ms, then one retry). The queue is cancelled at each title change.
11. **Title folder fix (v13.4, `Content.TitleFolders = 0` by default).** Level 1: an HDD folder `Content\<ID>\` gets an extra NAS handle. After the last HDD entry, one NAS query with the mask `<current title ID>` (or the 8-hex mask of XAM) adds max. 1 entry. Level 2: `Content\<ID>\<TitleID>\` is merged the same way with the mask `00000002`. If the HDD title folder is missing, XAM gets the NAS title folder, and the listing uses the mask `00000002`. If the HDD listing already has the name, there is no NAS query. Code: `TitleFolderOpen`, `QueryLevel`.

## Rules that you must not break

These come from real failures. Each one cost a test cycle.

1. **Pass all hook parameters as raw `unsigned __int64` values.** No type conversion. The Xbox 360 `NtQueryDirectoryFile` has **no** `FileInformationClass` parameter. A wrong type caused an endless listing (v6).
2. **A read without an offset (NULL `ByteOffset`) always goes to the NAS with the original parameters.** XAM sets the file position itself with a seek (the real offset was `0x971A`, not the end of the last read). Serving it from the cache gave wrong data, and XAM refused all packages (v9).
3. **Do not assume a file object address is unique.** The kernel reuses it after a handle closes.
4. **Never write to the NAS.** New files and folders stay on the HDD.
5. **xkelib's `IO_STATUS_BLOCK` has no `Status` member.** Use `IOSB_STATUS(p)` (offset 0).
6. **No 64-bit loads from addresses that are only 4-byte aligned** in kernel structures. Read two `DWORD` values.
7. **No locks, no `Log`, no notifications in a driver dispatch hook.** Only interlocked operations and memory copies.
8. **Keep the import patcher as it is** (`b .` stub write, then `g_FlushInsnCode` cache flush). It is proven.

## Facts from the tests

**DLC load (Guitar Hero II, 10 packages):** about 13 s without the cache, about 6 s with it. Per package: listing about 200 ms, mount about 500 ms (of which about 350 ms is the kernel STFS mount).

**Guitar Hero 5 (63 packages):** the listing takes about 23 s. The game opens each package **on demand**, when you select its song.

**The 16.6 ms delay.** Each NAS request from a XAM thread costs one frame (16.6 ms), also for 4 bytes. Large reads cost about one frame per 8 KB. The same reads from a **game thread** take about 0.6 ms (64 KB in about 7 ms). Without a running game (title update load at launch), all requests are fast. A higher thread priority does **not** help (v14d: 28.14 s vs 28.02 s). The cause is probably in the SMB filter driver.

**Listing time per package (v13.3 log, Guitar Hero 5, 63 packages, cache on, prefetch off).** First listing after the game start: 23.4 s (requests take 33 to 66 ms). Second listing: 11.9 s, about 190 ms = 12 frames per package. Per package, XAM does: listing query (NAS), open read/write (refused at once, no network), open read-only (NAS), 3 reads (`0-354`, cache fill = 1 NAS read), close, open again (NAS), 3 reads (up to `511`, all from the cache), close. Only 4 of the 12 frames are NAS requests. The other frames are XAM waits, also for reads from the cache. Thus the prefetch can save max. about 1 frame per package (about 1 s of 12 s). The target of 8 s is not possible with the cache or the prefetch alone.

**Folder walk (v13.3 log, ContentTrace).** At a DLC scan, XAM opens `Content\`, then `Content\<ID>\`, then `Content\<ID>\<TitleID>\`, then `...\<TitleID>\00000002\`. It opens the next level only if the folder exists: there is no failed open of a missing title folder. Thus XAM probably lists each level and looks for the next name. Version 13.4 logs the listings to prove this.

**Without the HDD title folder (v13.3 log).** At the Quickplay scan, XAM opens `Content\` and `Content\0000000000000000\`, and then stops. There is no open of `0000000000000000\41560840\`. Thus XAM decides from the content of `Content\0000000000000000\`.

**Listing format (v13.4 trace).** XAM lists each `\Content\` level with the mask `'*'` on the first call and `NULL` after it, one entry per call, buffer length `0x78`. It opens `<ID>\<TitleID>\` only for the title IDs in the listing.

**Title folder fix works (v13.4, `Content.TitleFolders = 1`).** Without the HDD folder `0000000000000000\41560840`, Guitar Hero 5 loaded all 63 NAS DLC packages, and the songs played.

**Prefetch has no gain (v13.4, 4 threads).** Worker requests take 33 to 200 ms each. They complete one frame apart, also when 4 run in parallel: the SMB path completes about one request per frame for the whole system. Listing time: 23.9 s with prefetch, 23.4 s without (first listing); 11.9 s both (second listing). Parallel requests do not help.

**Title folder problem.** Without `Hdd1\Content\0000000000000000\41560840\00000002\` on the HDD, Guitar Hero 5 never asked for this folder, and no DLC loaded. With the folder (one package in it), the merge worked. XAM probably checks a higher folder first. Workaround: create the empty folder. Fix: see `FOLLOWUP.md`.

**Title updates.** XAM does not list `000B0000`. It opens exact names on each device, for example `tu00000002_00000000` (Guitar Hero II) and `tu00000001_00000000` (Minecraft). Only lowercase `tu…` files work. Uppercase `TU_…` files (system cache) are not supported, and the trace never showed how they load.

**DLC detection.** XAM lists the folder and opens every file. It identifies a package by its header (reads `0` to `0x354`, then `0x411`), not by its name.

## Kernel facts (from diagnostic versions 14a to 14d)

- `\Network\Smb` is a **filter driver** in a module at about `0x91E80000`. It renames the real device to `\Network\RealSmb`. The module is not in `launch.ini`.
- File object `+04` = device object. Device object `+08` = driver object. Driver object `+0C` = `MajorFunction[]` (11 entries, index 2 = read).
- IRP: `+1C` buffer, `+50` stack location (`IRP + 0x60`), `+54` file object. Stack location: `+60` major function, `+64` length, `+6C` offset (64 bits), `+78` file object.
- A read hook works without code patches: copy the driver object, change entry 2 in the copy, set `device + 8` to the copy.
- The kernel STFS mount reads `0x344` (4 bytes), `0x971A`, and the 4 KB blocks `B6000`, `B000`, `C000`, `D000`.

Details: section "Findings for later work" in `README.md`.

## Files

| File | Content |
|---|---|
| `NasDlc/nasdlc.cpp` | Source, version 13.4 (13.2 + prefetch, listing trace and title folder fix, all off by default) |
| `NasDlc/NasDlc.xml` | XEX configuration |
| `NasDlc/NasDlc.ini` | Sample settings |
| `NasDlc.sln`, `NasDlc/NasDlc.vcxproj` | Visual Studio 2010 project |
| `README.md` | User documentation and kernel findings |
| `FOLLOWUP.md` | Next steps, with acceptance criteria |

Older and diagnostic versions are not in the repository. They were: v7 (first working redirect), v8 (read measurement), v9/v9b (cache failure and diagnosis), v10 (cache fix), v11 (title updates), v12 (INI, notifications), v13 (safety fixes), v13.1 (HDD log lines, log file lock), v14a–v14d (kernel probe, SMB read capture, priority boost).

## How to read a log

- `Level = 2` shows each `HDD …`, `NAS …`, `merge folder`, `cache … fill` and `close …` line.
- `NAS open … = C0000022` then `… access 00120089 = 00000000`: normal (read/write refused, read-only works).
- `NAS listing done: N DLC entries`: the NAS gave XAM N packages.
- `close <pkg>: … open for <many seconds>`: XAM accepted and mounted the package. A close soon after the reads means that XAM refused it.
- `C0000001` from the NAS during a game: the path does not exist (the SMB client gives this status instead of `C000003A`).

# FOLLOWUP — next steps for NasDlc

Status: version 13.2 is the release. DLC and lowercase title updates load from the NAS. Do the steps in this order. Each step needs a console test by the user.

## 1. Title folder fix (high priority)

**Problem.** Guitar Hero 5 (`41560840`) searches for DLC only if `Hdd1\Content\0000000000000000\41560840\00000002\` exists on the HDD. Without it, XAM never opens that path, so the plugin cannot redirect it. Workaround now: the user creates the empty folder.

**Step 1a: trace (no code change).**

1. On the HDD, move the folder `Content\0000000000000000\41560840` to a backup location.
2. In `NasDlc.ini`: `[Log] Level = 2` and `ContentTrace = 1`.
3. Start Guitar Hero 5, sign in, select the HDD as the storage device, open the Quickplay song list.
4. Read the `CT` lines. Find the path that XAM checks before `…\41560840\00000002\`. Probable candidates:
   - an open of `…\Content\0000000000000000\41560840\` (title folder),
   - an open or listing of `…\Content\0000000000000000\` (user folder), followed by a check for the title ID,
   - an attribute query (`NtQueryFullAttributesFile`) of the title folder.

**Step 1b: fix (depends on 1a).**

- If XAM opens or queries the **title folder**: extend `MapToNas` to accept `<16 hex>\<8 hex>` (no type) and `<16 hex>\<8 hex>\` for **folder opens and attribute queries only**. On HDD "not found", return the NAS result. Register no merge for this level.
- If XAM **lists** `Content\<ID>\`: merge the listing like the DLC listing (HDD title folders first, then NAS title folders, without duplicates). Only title ID names (8 hex) from the NAS.
- New INI setting `Content.TitleFolders = 1`, so the user can turn the fix off.
- Never create folders on the HDD from the plugin.

**Acceptance.**

- Without the HDD folder `41560840`, Guitar Hero 5 shows all NAS DLC songs, and the log shows `NAS listing done: 63 DLC entries`.
- Guitar Hero II (DLC only on the NAS) still works.
- A game without DLC on the NAS starts normally. No new NAS requests appear for games that have no NAS folder, apart from one failed check.

## 2. Parallel prefetch (medium priority)

**Problem.** The listing takes about 360 ms per package (Guitar Hero 5: about 23 s for 63 packages). Most of it is the 16.6 ms per request delay in XAM threads.

**Idea.** When the NAS listing returns a package name, put the path in a queue. 2 to 4 worker threads (system threads, like the watch thread) open the package read-only and fill the cache (`0-1000`) before XAM opens it. Then XAM's header reads come from memory.

**Unknowns, to measure first.**

- Do worker thread requests also wait one frame? The game thread did not, but the worker threads are system threads.
- Do parallel requests complete in the same frame?

**Rules.**

- INI setting `Speed.Prefetch = 0` by default.
- Workers use the original kernel functions (`g_pNtOpenFile`, `g_pNtReadFile`), read-only access (`00120089`), and close their own handles.
- A worker never blocks XAM: if a cache entry is busy, XAM reads from the NAS as now.
- Cancel the queue at each title change.

**Acceptance.** Log the listing time (first `NAS entry` to `NAS listing done`). Target for Guitar Hero 5: below 8 s. All songs show. No `MISMATCH` lines with `Cache.Verify = 1`.

## 3. Housekeeping (low risk)

- Make sure that the console runs the release build, not a diagnostic build (v14x).
- Remove the `[Debug]` and `[Speed]` sections from the user's `NasDlc.ini` (unless step 2 adds `Speed.Prefetch`).
- After step 1: update `README.md` (remove the title folder workaround, or describe the new setting).
- Publish (optional): add a license and a `.gitignore` (VS build output, `.xex`, logs). Ask the user to check that the use of xkelib and XDK headers is acceptable in a public repository.

## 4. Optional extensions

- **More content types** through the INI, for example `00009000` (avatar items). Do not add `00000001` (saves): they need writes.
- **Free path rules** in the INI (`HDD path → NAS path`) for folders outside `Content`.
- **Profile content**: already redirected (any 16-hex ID), but not tested.

## Parked (do not start without a new reason)

- **Driver-level read-ahead cache** (complete IRPs from the cache). Estimated gain: about 0.6 s per 10 packages. Risk: a console freeze. The kernel findings are in `README.md`.
- **Uppercase `TU_…` title updates.** The trace never showed how they load, and Aurora did not detect a copied file.
- **NAS as a storage device.** Needs block device emulation and a USB storage registration. Very slow because of the per-request delay.
- **Priority boost.** Tested in v14d. No effect.

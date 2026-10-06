# FOLLOWUP — next steps for NasDlc

Status: version 13.2 is the release. DLC and lowercase title updates load from the NAS. Do the steps in this order. Each step needs a console test by the user.

## 1. Title folder fix (high priority)

**Problem.** Guitar Hero 5 (`41560840`) searches for DLC only if `Hdd1\Content\0000000000000000\41560840\00000002\` exists on the HDD. Without it, XAM never opens that path, so the plugin cannot redirect it. Workaround now: the user creates the empty folder.

**Step 1a: trace.**

Result of the first trace (v13.3, with the HDD folder): XAM opens `Content\`, `Content\<ID>\`, `Content\<ID>\<TitleID>\` and `...\<TitleID>\00000002\`, but only folders that exist. There is no failed open of a missing title folder. Thus XAM probably lists `Content\<ID>\` and looks for the title ID. The open trace cannot show listings. Version 13.4 adds `CT list` lines (folder, mask, buffer length, status, names). Do the steps below with v13.4. The log must answer:

- Does XAM list `Content\0000000000000000\` (and `Content\0000000000000000\41560840\`)?
- Which mask does XAM use (`NULL`, `'*'`, or the title ID)?
- How many entries does XAM get per call? (The current merge works only with one entry per call.)

1. On the HDD, move the folder `Content\0000000000000000\41560840` to a backup location.
2. In `NasDlc.ini`: `[Log] Level = 2` and `ContentTrace = 1`.
3. Start Guitar Hero 5, sign in, select the HDD as the storage device, open the Quickplay song list.
4. Read the `CT` lines. Find the path that XAM checks before `…\41560840\00000002\`. Probable candidates:
   - an open of `…\Content\0000000000000000\41560840\` (title folder),
   - an open or listing of `…\Content\0000000000000000\` (user folder), followed by a check for the title ID,
   - an attribute query (`NtQueryFullAttributesFile`) of the title folder.

Result of the second trace (v13.3, without the HDD folder): at the Quickplay scan, XAM opens `Content\` and `Content\0000000000000000\`, and then stops. It never opens `0000000000000000\41560840\`. Thus XAM decides from the content of `Content\0000000000000000\`. Unknown: mask and entries per call. The v13.4 trace shows them.

**Step 1b: fix — implemented in v13.4 (`Content.TitleFolders`, off by default), not tested.** Design: see CLAUDE.md, architecture item 11. The fix does not depend on the mask or on the number of entries per call: the NAS query uses its own mask (the current title ID, or `00000002`), so it returns max. 1 entry.

**Test (one build, two boots), HDD folder `Content\0000000000000000\41560840` moved away:**

1. Boot 1: `TitleFolders = 0`, `ContentTrace = 1`, `Level = 2`. Open the Quickplay song list. Keep the log (`CT list` lines = the trace for step 1a).
2. Boot 2: `TitleFolders = 1`, same other settings. Open the Quickplay song list. Look for `title folders:` lines and `NAS listing done: 63 DLC entries`.
3. Boot 3 (regression): put the HDD folder back, `TitleFolders = 1`. Guitar Hero 5 must still show all songs.

**Original plan for step 1b (for reference).**

- If XAM opens or queries the **title folder**: extend `MapToNas` to accept `<16 hex>\<8 hex>` (no type) and `<16 hex>\<8 hex>\` for **folder opens and attribute queries only**. On HDD "not found", return the NAS result. Register no merge for this level.
- If XAM **lists** `Content\<ID>\`: merge the listing like the DLC listing (HDD title folders first, then NAS title folders, without duplicates). Only title ID names (8 hex) from the NAS.
- New INI setting `Content.TitleFolders = 1`, so the user can turn the fix off.
- Never create folders on the HDD from the plugin.

**Acceptance.**

- Without the HDD folder `41560840`, Guitar Hero 5 shows all NAS DLC songs, and the log shows `NAS listing done: 63 DLC entries`.
- Guitar Hero II (DLC only on the NAS) still works.
- A game without DLC on the NAS starts normally. No new NAS requests appear for games that have no NAS folder, apart from one failed check.

## 2. Parallel prefetch (medium priority)

**Status: implemented in v13.3, not tested on the console.** Each test step below needs a log.

**Expected gain (from the v13.3 log): small.** Per package, XAM uses about 12 frames, but only 4 are NAS requests. The prefetch removes only the cache fill (1 frame). Expected: about 11.9 s → 10.9 s for 63 packages. The target of 8 s is not possible this way. Priority: low. Do step 1 first.

**Test plan.**

1. `[Log] Level = 2`, `[Cache] Verify = 1`, `[Speed] Prefetch = 1`, `PrefetchThreads = 2`.
2. Start Guitar Hero 5. Open the Quickplay song list.
3. Send `NasDlc.log`. Look at these lines:
   - `prefetch: worker N listed M packages in X ms (R requests, ...)`: the listing speed of a worker thread. `R` near `M` means one entry per request. `last 80000006` is the normal end.
   - `prefetch: worker N <pkg>: open X us, read = ..., Y us`: the request time of a worker. About 16600 us = one frame (the delay applies to workers too). About 600 us = no delay.
   - `prefetch done: ... max P parallel requests`: `P` > 1 and short times mean that parallel requests are possible.
   - `NAS listing done: 63 DLC entries in X ms, N from the prefetch`: the result. `N` near 63 means that XAM used the prefetched data.
   - `MISMATCH`: must not occur.
   - `prefetch: sharing conflict`: must not occur. If it occurs, send the log.
4. Repeat with `PrefetchThreads = 4` and with `Prefetch = 0`, and compare the `NAS listing done` times.
5. Check that all songs show and Guitar Hero II still loads its DLC.

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

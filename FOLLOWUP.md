# FOLLOWUP — next steps for NasDlc

Status: version 13.5 is the release. DLC and lowercase title updates load from the NAS. Do the steps in this order. Each step needs a console test by the user.

## 1. Title folder fix — done (v13.5)

XAM lists `Content\` and each `Content\<ID>\` (mask `'*'`, then `NULL`, one entry per call). It opens `<ID>\<TitleID>\` only for the title IDs in the listing. Without the HDD title folder, it never asked for the DLC folder.

Fix: `Content.TitleFolders` (on by default). Design: CLAUDE.md, architecture item 10. Tested in v13.4:

- Without the HDD folder `0000000000000000\41560840`: Guitar Hero 5 loaded all 63 NAS DLC packages, and the songs played.
- With DLC on the HDD and on the NAS (mixed): still works.

Cost: one failed NAS open (16 to 33 ms) for each other profile folder (`E000…`) per scan.

## 2. Parallel prefetch — tested, removed (v13.5)

Tested in v13.4 with 4 worker threads. No gain: first listing 23.9 s (without: 23.4 s), second listing 11.9 s (without: 11.9 s). Worker requests took 33 to 200 ms each. They completed one frame apart, also in parallel: the SMB path completes about one request per frame for the whole system. During the prefetch, XAM's own opens took longer. Do not try parallel requests again.

## 3. Housekeeping (low risk)

- Make sure that the console runs the release build, not a diagnostic build (v14x).
- Remove the `[Debug]` and `[Speed]` sections from the user's `NasDlc.ini` (no longer used).
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
- **Parallel requests (prefetch).** Tested in v13.4. No effect (one request per frame for the whole system).

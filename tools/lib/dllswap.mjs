// ---------------------------------------------------------------------------
// dllswap.mjs - replacing a DLL the game currently has loaded.
//
// Windows refuses to overwrite or delete a loaded DLL: the loader has its
// contents mapped. It does *not* refuse to RENAME one - the lock is on the
// bytes, not on the name. So the running game keeps executing what it already
// mapped, under a new name, while the new build takes the old name and is
// what the next launch loads.
//
// That is the ordinary Windows self-update dance, and it is why "close the
// game first" was never actually necessary. What it costs is a leftover file
// that cannot be deleted until nothing has it mapped, so every install sweeps
// up the ones left by previous runs.
//
// Verified against a DLL genuinely mapped into a live process: the overwrite
// is refused, the rename is not, the moved-aside copy stays intact for the
// process still running on it, and it deletes once that process exits.
// ---------------------------------------------------------------------------

import { existsSync, copyFileSync, unlinkSync, renameSync,
         readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';

const STALE_PREFIX = 'dxgi.dll.old-';

// Numbered rather than timestamped, so two reinstalls in the same second do
// not pick the same name.
function staleName(dir) {
  for (let i = 0; i < 1000; i++) {
    const p = join(dir, `${STALE_PREFIX}${i}`);
    if (!existsSync(p)) return p;
  }
  return null;
}

// Deletes what previous hot installs left behind. Each one only goes once the
// process that had it mapped has exited, so a failure here is expected and
// silent: it means the game is still running, and the next run gets it.
export function sweepStale(dir) {
  let gone = 0;
  try {
    for (const f of readdirSync(dir)) {
      if (!f.startsWith(STALE_PREFIX)) continue;
      try {
        unlinkSync(join(dir, f));
        gone++;
      } catch { /* still mapped; next time */ }
    }
  } catch { /* an unreadable directory is the caller's problem, not ours */ }
  return gone;
}

// Puts `dll` at `target`, whether or not something holds the old one open.
// Returns 'replaced', or 'hot' when the old one had to be moved aside - which
// the caller must report, because a hot swap does not take effect until the
// game restarts. Throws for any failure that is not the lock.
export function installDll(dll, target) {
  try {
    copyFileSync(dll, target);
    return 'replaced';
  } catch (e) {
    // Anything but the lock we know how to work around is a real failure - a
    // read-only folder, a full disk - and must not be dressed up as one.
    if (!existsSync(target) || !['EBUSY', 'EPERM', 'EACCES'].includes(e.code)) {
      throw e;
    }
    const aside = staleName(dirname(target));
    if (!aside) throw e;
    renameSync(target, aside);
    try {
      copyFileSync(dll, target);
    } catch (e2) {
      // Put it back: leaving the game with no dxgi.dll at all is a worse
      // outcome than not updating it.
      try { renameSync(aside, target); } catch { /* nothing left to try */ }
      throw e2;
    }
    return 'hot';
  }
}

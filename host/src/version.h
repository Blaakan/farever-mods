// ---------------------------------------------------------------------------
// The one place the version number lives.
//
// The host logs it on attach, so `farever-modkit.log` from a stranger's
// machine says which build they are running - which is the first question
// about any bug report and the one nobody can answer from memory.
//
// tools/package.mjs reads this file to name the release archive, so bumping
// it here is the whole job.
// ---------------------------------------------------------------------------
#pragma once

#define FMK_VERSION "0.9.0"

# Rut deformation diagnostics — Phase 0

- [x] Inspect the current hook, signature-relocation, opt-in diagnostic, logging, and test patterns.
- [x] Add beta21e deformation-writer/apply constants and unique signatures with invariant tests.
- [x] Add default-off, marker-file-gated observation hooks that fail closed to stock behavior.
- [x] Log native call ownership, thread, coordinates/magnitude, four-cell pre/post values, dirty blocks, and received-block application.
- [x] Add rate limiting and bounded summary logging without changing terrain values.
- [x] Document exact enable, install, smoke-test, two-client test, and rollback steps.
- [x] Build and run the relevant portable tests; verify Windows source with the available cross-toolchain.
- [x] Inspect the public diff for private-map leakage and hand off for manual review.

Database impact: none. This phase changes no schema, migration, backfill, or index.

## Review / results

- The diagnostic defaults off and is enabled only with `frostmod.exe --rut-diag`.
- A beta21e timestamp mismatch or either exact-signature mismatch refuses that hook and leaves stock behavior.
- Writer logging samples the first 48 calls then every 256th; apply logging samples the first 24 blocks then every 16th so the join sweep cannot hide a short riding test.
- The apply observer records signed input summaries, authoritative-cell changes, and 16-bit height changes for the watching-client test.
- All 20 portable CTest targets pass on macOS. MinGW syntax checking passes after substituting SEH only for parsing; the available MinGW compiler cannot build the MSVC-SEH DLL itself.
- Direct warning-enabled `offsets_test` and warning-as-error `rutdiag_test` builds pass after the final block-count bounds change.
- A PE32+ x64 launcher artifact was cross-built at `/private/tmp/frostmod-rut-diag-artifacts/frostmod.exe`; the DLL is buildable with the documented Windows/MSVC commands.
- A real Windows run exposed a stale pre-diagnostic `frostmod.exe` that consumed
  `--rut-diag` as a DLL path. The guide now treats that output as an artifact mismatch and
  requires rebuilding/copying `frostmod_app`; an explicit DLL positional argument is not a
  valid substitute for a launcher that does not implement the diagnostic flag.
- Public changed-file scan found no private repo path, private toolkit, unpacker, or game binary.
- No database changes. No commit, push, PR, or changelog edit was made.

## Phase 1 — bounded negative pulse

- [x] Add a separate `--rut-negative-pulse` launcher marker; `--rut-diag` alone must remain observation-only.
- [x] Add pure candidate selection and one-shot state rules with refusal-path tests.
- [x] After an accepted stock writer call, inject one `-0x00040000` cell only when an adjacent terrain cell belongs to an entirely zero, clean block.
- [x] Preserve the stock writer exactly once and never touch authoritative/rendered terrain or packet buffers.
- [x] Force apply-side evidence for the source rider and an optional later watcher, including source-pulse correlation when available.
- [x] Document rebuild, required solo success/failure, optional fanout confirmation, rollback, and the fact that these changes remain uncommitted until manual approval.
- [x] Run portable CTest, strict-warning focused tests, Windows launcher/DLL syntax checks, diff checks, and a public leakage review.

Database impact: none. This phase changes no schema, migration, backfill, or index.

### Phase 1 review / results

- `--rut-diag` alone never creates the pulse marker. `--rut-negative-pulse` creates a
  separate marker and implies observation so the timestamp, writer signature, apply signature,
  and apply evidence are all prerequisites.
- The stock writer is called exactly once before the pulse decision. The one-shot waits for
  four positive stock-cell changes, then selects only a cardinally adjacent terrain-interior
  cell outside the stock blocks whose complete outbound block is zero and whose dirty byte is
  clear. It assigns `-262144` to the zero cell and marks that one block.
- The process-wide atomic latch prevents a second fire across calls, reloads, and later
  sessions. Failed/revalidated candidates release the claim without writing; a guarded memory
  fault closes the latch rather than risking a retry.
- Every received block is inspected while the manual diagnostic hook is active so a negative
  cell forces evidence even between ordinary rate-limited samples. The required solo
  local-host test proves one-shot firing, the local client/server round trip, exact
  authoritative delta, positive height response, stability, and no repeat after rejoin/reload.
  A later watcher can expose the same cell/index/block and height result for optional remote
  fanout confirmation; that limitation is recorded but is not a blocker for the next bounded
  shape experiment after solo success.
- The future continuous-rut acceptance criteria are documented but intentionally not built:
  reasonably smooth starting tracks; rider-generated believable roughness; shallow centers;
  rounded multi-cell shoulders; smoothing; conservative accumulation; hard depth/slope caps;
  natural behavior across dirt types; and no one-cell cliffs, exaggerated trenches, or janky
  low-speed catches.
- All 20 portable CTest targets pass. Warning-enabled `offsets_test` and warning-as-error
  `rutdiag_test` pass. MinGW validates the Windows DLL source with the existing SEH parsing
  substitution, and the launcher cross-build is a PE32+ x64 executable containing both rut
  flags and the separate pulse marker.
- `git diff --check` and the added-line public leakage scan pass. No private map, toolkit,
  binary, or local path is included.
- No database changes. No commit, push, PR update, or changelog edit was made for Phase 1.

## Phase 1b — inject at outbound serialization

- [x] Correlate the failed solo log with the private beta21e sender map and identify the
  exact pre-deflate row boundary already used by the outbound world-data serializer.
- [x] Separate safe candidate selection/arming from the actual negative-cell write.
- [x] Inject exactly once only when the selected target row reaches the mapped serializer.
- [x] Log an explicit source exact/mismatch/missing result whenever the target block returns.
- [x] Add focused pure tests for arm/inject separation, wrong-block no-op, race refusal,
  one-shot behavior, and source-result classification.
- [x] Keep the solo test guide compact and align it with the delayed injection lifecycle.
- [x] Run the portable, sanitizer, Windows syntax/cross-build, diff, leakage, and stock-path
  invariant checks; document manual-review results without committing or publishing.

Database impact: none. This phase changes no schema, migration, backfill, or index.

### Phase 1b review / results

- The accepted-stock-writer hook now only selects a still-zero cell in a clean neighboring
  block, records the target, and queues that block. It does not write the negative value.
- A separately signature-gated hook on the stock serializer's raw-deflate function accepts
  only the mapped terrain-row call. Wrong rows call stock unchanged. On the target row it
  atomically claims the armed pulse, revalidates the saved geometry, cell, dirty byte, and
  complete zero block, then assigns exactly `-262144` immediately before the original
  deflate call consumes the row. A dirty/nonzero/invalid race writes nothing and permits a
  later safe selection; a guarded access fault closes the one-shot.
- The apply observer explicitly samples the saved target whenever its block returns, even
  when the value is positive or zero. It emits `source-match`, `source-mismatch`,
  `source-missing`, or `source-unavailable`, with requested/input/authoritative/height values.
- Disabled behavior is unchanged: observation-only mode never installs the serializer hook,
  and pulse mode remains off unless the beta21e timestamp plus writer, apply, deflate, and
  mapped serializer-call validations all pass. Each detour calls its stock original exactly
  once; authoritative/rendered terrain and compressed packet bytes are never edited.
- All 20 portable CTest targets pass. Warning-as-error ASan/UBSan `rutdiag_test`, strict
  `offsets_test`, MinGW DLL syntax checking, and the PE32+ x64 launcher cross-build pass.
  `git diff --check` and the public added-line leakage scan pass.
- No private map file was changed because the sender, deflate boundary, and post-send clear
  behavior were already decoded. No database, changelog, commit, push, or PR change was made.

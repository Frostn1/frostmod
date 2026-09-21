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

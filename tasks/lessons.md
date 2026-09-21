# Lessons

## Verify the artifact the user actually runs

- Do not infer distributed Windows behavior from the current source tree, a cross-build, or
  strings embedded in a locally built executable alone.
- When command-line parsing is under test, exercise the exact executable delivered to the
  user, or first verify its build banner/hash and that it was copied from the intended build
  output.
- Treat an option being consumed as a positional path as evidence of an artifact/version
  mismatch until the actual executable's parser is inspected. Do not propose argument-order
  workarounds without testing them against that exact parser generation.

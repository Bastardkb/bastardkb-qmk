# Agent guidance (this repository)

This is the `bastardkb-qmk` tree. Project workflow lives in `docs/superpowers/` — read
`docs/superpowers/phase-execution-instructions.md` at the start of every phase, and check
`docs/superpowers/qmk-knowledge-base.md` before researching any QMK behaviour.

## Always-apply rules

- **Never run git** (`status`, `log`, `diff`, `commit`, `checkout`, ...). Version control is not part of
  this workflow; it is not evidence and must not be used for orientation or verification.
- **Build/compile once per phase artefact.** A successful build proves a clean compile; do not recompile
  to re-derive or re-check it, warnings included.
- **Run each check once.** Do not re-run a command or re-verify evidence a build, a `STATIC_ASSERT`, or an
  earlier run already proved.
- **Before any command, ask whether it is required for this phase's single deliverable.** If not, skip it.

The full rule is in `docs/superpowers/phase-execution-instructions.md` ("Tooling discipline").

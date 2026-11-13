# Agent guidance (this repository)

This is the `bastardkb-qmk` tree. **Read this file at the start of every task.** It encodes how we work
together. Then read, in order:

- `docs/superpowers/plans/*.md` — the plan (objective, architecture, ordered steps). This is the working
  contract.
- `docs/superpowers/qmk-knowledge-base.md` — our researched QMK findings. **Check it before researching**
  anything about QMK/this repo; if you learn something new, add it there.

## How we approach work

1. **Start fresh.** Read this file, then the relevant plan, then the knowledge base. Do not carry
   assumptions from previous work that aren't written down.
2. **Keep it simple.** Do the minimum walkable deliverable for the step, nothing more. If something grows
   beyond the step's scope, stop and talk to the user.
3. **One step at a time.** Implement ONE plan step, validate it on hardware, then **STOP**. Do not start the
   next step on your own.
4. **One variable per step.** If something worked and then didn't, **bisect before adding anything** —
   never change two things (e.g. boot behaviour and the transport) in the same step.

## Tooling discipline (always apply)

- **Never run git** (`status`, `log`, `diff`, `commit`, `checkout`, ...). Version control is not part of
  this workflow; it is not evidence and must not be used for orientation or verification.
- **Build/compile once per artefact.** A successful build proves a clean compile; do not recompile to
  re-derive or re-check it, warnings included.
- **Run each check once.** Do not re-run a command or re-verify evidence a build, a `STATIC_ASSERT`, or an
  earlier run already proved.
- **Before any command, ask whether it is required for this step's single deliverable.** If not, skip it.
  Prefer reading files over running tools.
- **Search inside WSL, not over the share.** The tree is a WSL share, so Windows-side search tools walk the
  whole tree over a network FS and are slow. Use `wsl -d Ubuntu -- bash -lc 'rg -n <pattern> <path>'`.

## Ask the user — don't assume

- **Don't hesitate to ask the user questions. Don't make assumptions about their intentions.**
- When a decision matters (scope, numbering, recipients, tools, plan wording), ask with `question` and a
  recommended option first, explicitly marked "(Recommended)".
- If while implementing you find something that could be better or differs from the plan, **ELI5 the finding
  to the user, ask to confirm, and modify the plan on the go only after they agree.**
- A plan is a working contract, not a straitjacket. Changing it after asking is normal and welcome.
- **When in doubt, ask a question that unblocks the next decision** — request evidence or clarify scope
  (e.g. "paste the exact raw console/build output", or "is doing X part of this step's scope?"). Do not
  proceed on a guess when the answer would change the work.

## Stuck / repeated attempts — stop and talk

- If you try to accomplish the **same thing** two different ways and neither worked, **stop**. Do not keep
  switching approaches on your own.
- Explain ELI5: what you're trying to do, why, and the approaches you already tried and why each failed.
  Ask the user for advice or a recommended path before continuing.
- Two attempts is the limit for one objective within a single stretch of work. A genuinely new idea after
  asking is a fresh attempt with the user's blessing, not a silent third try.

## Gate evidence & checklists — build efficiency

- **Consume-only evidence.** A checklist should ask only for evidence a downstream decision or the
  validation actually uses. Don't request a value just because it *feels* required.
- **Proved-by-construction.** If a constraint is enforced by a passed compile-time gate (`STATIC_ASSERT`,
  `#error`, a linker error), it is already proven by a successful build. Do not spend effort re-deriving it.
- **Verify observability before the gate, not at it.** Before handing over a gate, confirm the expected
  evidence actually appears in the intended channel (e.g. `qmk console`) with no hidden toggle. If a message
  needs device debug enabled, either switch to unconditional logging (`print`/`xprintf`) or state the toggle
  explicitly in the instructions.

## Keeping the plan in sync

- Update the plan file as decisions are made: correct typos, note deviations, retire caveats, record
  confirmed knobs. Small factual fixes can be applied directly; behaviour/scope changes need the user's
  confirmation first.

## Self-review before finishing

Before you declare a step done, review your own work:

- Re-read the file(s) you changed: check for leftover/dead code, obsolete knobs, stale comments, temporary
  debug scaffolding, and anything redundant from earlier work. Clean it up.
- Confirm the change matches the plan's step and touches nothing outside its scope.
- Confirm the deliverable builds clean (compile once) and that pure logic has a native test where practical.
- Confirm the expected console evidence actually appears in the intended channel.

## Handoff to the user (every deliverable)

- Every step delivers exactly **ONE artefact** (typically one UF2) and **STOPS at the human validation
  gate**.
- Before finishing, hand the user — explicitly and in your final message:
  - the exact build command and the artefact path;
  - what the artefact is supposed to do;
  - the exact test instructions: which halves to flash, which USB to connect, how to power-cycle, what to
    capture (the full console from power-on) and for how long, and the single marker/line to look for;
  - what **PASS** and **FAIL** look like.
- Do not finish with only code changes and no testable artefact + instructions. On the user's PASS, deliver
  a short retrospective (what went well / badly, a TLDR) and update the plan before the next step.

## Process artefacts in this folder

- `docs/superpowers/plans/` — plan documents (the working contract).
- `docs/superpowers/qmk-knowledge-base.md` — our researched QMK findings.

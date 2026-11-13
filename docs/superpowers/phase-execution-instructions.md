# Phase Execution Instructions

Read this file at the **start of every new phase** (before starting work). It encodes how we work together:
rules for asking questions, changing plans, the QMK knowledge base, keeping things simple, and how each
phase ends (retrospective + plan update).

## How to approach a new phase

1. **Start fresh.** Read this file, then the plan (`docs/superpowers/plans/*.md`) section for the phase
   you are starting, plus the design doc (`docs/superpowers/specs/*.md`) if referenced. Do not carry
   assumptions from previous phases that aren't in the plan.
2. **Keep it simple.** Scope is deliberately small per phase. Do the minimum walkable deliverable for the
   phase, nothing more. If something grows beyond the phase scope, stop and talk to the user.
3. **The QMK knowledge base first.** When you need to learn or research how QMK/QMK configuration/this
   codebase works:
   - First read `docs/superpowers/qmk-knowledge-base.md` to see if we already researched it.
   - If not, research it manually, **add your findings to the knowledge base doc**, then proceed.
   - Do not re-research something already documented; trust and verify quickly instead.

## Ask the user — don't assume

- **Don't hesitate to ask the user questions. Don't make assumptions about their intentions.**
- When a decision matters (scope, numbering, recipients, tools, plan wording), ask with
  `question` and a recommended option first, explicitly marked "(Recommended)".
- If while implementing you find something that could be better or differs from the plan, ELI5 the
  finding to the user, ask to confirm, and modify the plan on the go **only after they agree**.
- A plan is a working contract, not a straitjacket. Changing it after asking is normal and welcome.
- **When in doubt, ask a question that unblocks the next decision — request evidence or clarify scope.**
  Examples: "paste the exact raw console/build output", or "is doing X part of this phase's scope, or
  yours to handle?" Do not proceed on a guess when the answer would change the work.

## Stuck / repeated attempts — stop and talk

- If you try to accomplish the **same thing** two different ways and neither worked, **stop**. Do
  not keep switching approaches on your own.
- Explain ELI5: what you're trying to do, why, and the approaches you already tried and why each
  failed. Ask the user for advice or a recommended path before continuing.
- Two attempts is the limit for one objective within a single stretch of work. If a genuinely new
  idea arrives after asking, that is a fresh attempt with the user's blessing, not a silent third try.
- This keeps effort focused and avoids burning time on one problem in ever-shifting ways (e.g. five
  flavours of getting the same number out of a build).

## Gate evidence & checklists — build efficiency

- **Consume-only evidence.** A gate checklist should ask only for evidence a downstream decision or the
  validation actually uses. Don't request a value/number just because it *feels* required.
- **Proved-by-construction.** If a constraint is enforced by a passed compile-time gate (`STATIC_ASSERT`,
  `#error`, a linker error), it is already proven by a successful build. Do not spend effort re-deriving
  or "extracting" its value unless a downstream consumer needs it (e.g. Phase 0's `NUM_TOTAL_TRANSACTIONS
  <= 32` was proven at link; the exact integer was not consumed by anything).
- **Verify observability before the gate, not at it.** Before handing over a gate, confirm the expected
  evidence actually appears in the intended channel (e.g. `qmk console`) with no hidden toggle. If a
  message needs device debug enabled, either switch to unconditional logging (`print`/`xprintf`) or state
  the toggle explicitly in the checklist.

## Keeping the plan in sync

- Update the plan file as decisions are made: e.g. correct typos, note deviations, retire caveats,
  record confirmed knobs. Small factual fixes can be applied directly; behavior/scope changes need the
  user's confirmation first.
- Each phase ends with an **end-of-phase retrospective and a plan update** (see below) after the user
  confirms PASS.

## End of phase

- Every phase delivers exactly one artefact (typically one UF2) plus a validation checklist, then
  **STOPS at the human validation gate**. Do not start the next phase on your own.
- On the user's PASS, deliver the end-of-phase retrospective:
  - what went well / what went badly
  - a TLDR for the user
- **Leftover/redundancy review before every next phase.** Each end-of-phase review also includes a
  code-review pass over all the work delivered so far (module code, config, host tests, plan text)
  checking for anything redundant or leftover from earlier phases: temporary test hooks, obsolete knobs,
  dead code paths, no-longer-true comments, superseded protocol handling. If anything is found, clean it
  up, rebuild, and the human re-runs the affected validation gate before the next phase starts — the next
  phase does not begin until that re-validation passes.
- Once the user confirms, update the plan's workflow contract / checklists before the next phase.

## Process artefacts in this folder

- `plans/` — phase plan documents (the working contract).
- `specs/` — design documents.
- `qmk-knowledge-base.md` — our researched findings about how QMK works (check before researching).

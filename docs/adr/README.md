# Architecture Decision Records

Decisions that shape PipeShellX's public contracts, security defaults,
dependencies, or layering are recorded here so that the *why* survives the
code that implements it. The index is the list below; `PLAN.md` §3 references
these records by number.

| ADR | Title | Status |
|---|---|---|
| [ADR-001](ADR-001-completion-style-runtime-api.md) | Completion-style runtime API over readiness and completion backends | Accepted |
| [ADR-002](ADR-002-system-openssh-as-agentless-transport.md) | System OpenSSH as the agentless transport | Accepted |
| [ADR-003](ADR-003-single-coordinator-no-consensus.md) | Single coordinator per run; no consensus protocol | Accepted |
| [ADR-004](ADR-004-openssl3-static-for-tls.md) | OpenSSL 3, statically linked, for the native backplane's TLS | Accepted (revisit in Phase 4 spike) |
| [ADR-005](ADR-005-result-type-in-lower-layers.md) | `psx::Result<T>` in L0–L2, exceptions permitted from L4 up | Accepted |

## Writing a new record

Copy the template below into `docs/adr/ADR-NNN-short-title.md` (next free
number), add a row above, and reference the ADR from the PR description.
Records are immutable once accepted: to change a decision, write a new ADR
that supersedes the old one and mark the old one `Superseded by ADR-NNN`.

```markdown
# ADR-NNN: Title

- **Status:** Proposed | Accepted | Superseded by ADR-MMM | Deprecated
- **Date:** YYYY-MM-DD
- **Deciders:** who
- **Related:** PLAN.md §x.y, other ADRs

## Context
What forces are at play; what the alternatives were.

## Decision
What we do, stated in the imperative.

## Consequences
What becomes easier, what becomes harder, what we must now guarantee.
```

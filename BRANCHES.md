# Branch guide

Orientation for everyone (humans and coding agents) working in this repository.
If you change the topology, update this file in the same PR/commit.

```
main                          <- release line. Protected: no force-push, no deletion.
└─ integration/redesign-v3    <- frozen neutral baseline for the GUI redesign. PR-only.
   ├─ experiment/gui-claude   <- GUI redesign track A (Claude Code). Active development.
   └─ experiment/gui-codex    <- GUI redesign track B (Codex). Active development.

archive/redesign-p1p3         <- read-only archive of the completed wizard redesign
                                 (phases P1-P3). Locked: no pushes of any kind.
```

## What each branch is for

| Branch | Role | Rules |
|---|---|---|
| `main` | The release line. CI-gated; releases are tagged from here. | Direct pushes allowed (bots + hotfixes). Force-push and deletion blocked by ruleset. |
| `integration/redesign-v3` | The frozen baseline both experiment tracks measure from. It only changes via small, agreed cross-track fix PRs (example: PR&nbsp;#3, the design-probe PNG fix). | PR-only, no force-push, no deletion (ruleset). Do **not** develop here. |
| `experiment/gui-claude` | GUI redesign, track A. One of two independent takes on the redesign, developed in parallel for comparison. | Free development by its owner only. |
| `experiment/gui-codex` | GUI redesign, track B. Same baseline, independent direction. | Free development by its owner only. |
| `archive/*` | Immutable snapshots (currently `archive/redesign-p1p3`, the completed P1-P3 wizard the baseline was cut from). | Fully locked by ruleset — the `archive/**` prefix is auto-locked, so future archives inherit protection. |

## The experiment ("bake-off")

Both `experiment/*` branches fork from the **same commit** of `integration/redesign-v3`
(identical starting pixels, including all shared fixes). Each track develops its own
version of the GUI independently. When both are finished, the better one is merged to
`main` via a normal merge; the other remains preserved on its branch. Nothing is deleted
by choosing.

Shared, non-GUI fixes (tooling, test infrastructure) should be offered to **both** tracks
via a small PR to `integration/redesign-v3` — never developed divergently in each track.

## House rules on every branch

- Test serial numbers are always the `000-0000` / `0000000` placeholders. A serial-guard
  CI workflow scans every push; a local pre-commit hook is available:
  `git config core.hooksPath tools/git-hooks`
- The full test suite must be green before pushing (`ctest --test-dir build`).
- No force-pushes anywhere. History rewrites are owner-coordinated operations only.

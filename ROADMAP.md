# EARS Bridge — Roadmap

Planned and proposed work, roughly in priority order. This is direction, not a commitment to
dates. Each substantial item gets its own spec + plan under
[`docs/superpowers/`](docs/superpowers/) before it's built.

## In progress / specced

- **Update notification** — background check of GitHub's latest release on launch → a subtle,
  non-modal "Update available · vX.Y.Z →" link in the title bar; on by default, with an Advanced
  opt-out, never auto-downloads (the link opens the release page). 24 h-throttled, fail-silent.
  → [spec](docs/superpowers/specs/2026-06-16-update-notification-design.md) ·
  [plan](docs/superpowers/plans/2026-06-16-update-notification.md)

## Next — app shell & onboarding

A cohesive bundle: give the app the standard desktop "chrome" it currently lacks (today it's a bare
window with no menu/About/Help) and make the first-run workflow self-explanatory. The pieces build on
each other, so they're best done as one epic.

- **Menu bar** — a native application menu. macOS: the system menu bar via `juce::MenuBarModel` +
  `MenuBarModel::setMacMainMenu`. Windows: an in-window `MenuBarComponent`. This is the foundation
  that hosts everything below.
- **About box** — the app icon, version + build, license + credits, and links. Reachable from the
  macOS app menu ("About EARS Bridge") and from Help. Sources its version from the same single source
  as the update check (CMake `PROJECT_VERSION`).
- **Help menu** — Documentation, GitHub repository, Report an issue, Releases, and **Check for
  updates…** (a manual trigger of the update-checker unit already specced above).
- **In-app links** — quick actions the app opens in the browser / OS, surfaced in the **Help menu**
  and/or the **About box**: Documentation (README / site), GitHub repo, Report an issue (prefilled
  new-issue URL), Releases, **Get VB-CABLE**, **Open Windows Sound settings** (`mmsys.cpl`), Dirac.
  The app already detects when VB-CABLE is missing, so the link closes that loop.
- **"What's new" / release notes** — on the first launch *after* an update, show the new release's
  notes once (non-modal, dismissible). Reuses the update checker, which already fetches the release —
  just surface the release `body`.
- **First-run setup guide** — a guided checklist for new users: ① install VB-CABLE ② plug in the
  EARS ③ load the per-ear calibration files ④ set Dirac to shared mode. The app *already detects*
  each of these states (cable present, EARS present, cals loaded, Dirac shared-mode env var), so this
  surfaces what it already knows. Shown on first run; re-openable from Help.
- **Tooltips** — hover help on the controls (input/output pickers, combine mode, rate/depth, output
  trim, Start). The theme already styles `TooltipWindow` — this just adds a `TooltipWindow` instance
  and `setTooltip(...)` on each control.

### How these fit together

- The **menu bar** is the foundation; **About** and **Help** hang off it; the **in-app links** live
  in Help (and About).
- **Check for updates** in the Help menu shares the update-checker unit from the specced feature above.
- **What's new** reuses that same fetch (add the release-notes `body` to its result).
- The **first-run guide** reuses the existing detection (VB-CABLE / EARS / cals / Dirac shared mode).
- Suggested order: **menu bar → About + Help + in-app links → tooltips → what's new → first-run guide.**

## Other candidates (raised, not yet prioritized)

- **Export diagnostics** — one-click "Save diagnostics…" support file (version, device list, selected
  config, health flags, Dirac shared-mode state); an in-app version of the `eb_diag` CLI.
- **Headphone presets / profiles** — save and load a named configuration (input + output + combine +
  rate + depth + cal pair) per headphone.
- **Window size/position memory** + **keyboard shortcuts** (e.g. Space = Start/Stop).
- **Code signing + notarization** — removes the SmartScreen (Windows) and Gatekeeper (macOS)
  "unidentified developer" warnings on first launch. Needs paid certificates (Apple Developer ID; a
  Windows OV/EV cert), so it's a cost/process decision, not just code.

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

## Measurement integrity — clipping & stream audit

A [full audit](docs/EARS_DIRAC_CLIPPING_AUDIT.md) found the app could show a green "Running · clean"
verdict over a clipped, corrupted, or mistimed Dirac sweep. The first slice — confirmed-clip detection
that actually **invalidates** the measurement, a NaN/Inf guard, one honest clip threshold, and honest
status wording (audit findings D1, D3, D4) — is done on `feat/confirmed-clipping-detection`
→ [plan](docs/superpowers/plans/2026-06-16-confirmed-clipping-detection.md). What remains, by priority:

### Red — blocks the "no digital clipping" claim

- **Raw-rail capture (D2)** — the EARS opens in WASAPI/CoreAudio *shared* mode with the OS resampler
  armed (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), so the app measures OS-resampled/mixed float, not the
  device's true digital rails. Pin the EARS to its native rate and assert no SRC engages (or open it
  exclusive for the measurement); document the residual shared-mixer caveat. This is the structural one.
  → [plan](docs/superpowers/plans/2026-06-16-d2-raw-rail-capture.md)
- **Sweep / measurement session (D5)** — clipping and integrity are scoped to the Start→Stop *engine
  run*, not Dirac's sweep, so pre- and post-sweep room events fold into the result and the "on the
  sweep" wording is unprovable. Add an idle → preflight → sweep-active → complete/invalid state machine
  that scopes the latch to the active sweep window.
  → [plan](docs/superpowers/plans/2026-06-16-d5-measurement-session.md)
- **Frozen resample ratio during the sweep (D6)** — the clock-bridge PI controller republishes the SRC
  ratio every render block; continuous sub-0.5 % creep nonuniformly retimes the log sweep *below* the
  drift threshold, so it reads clean. Estimate the ratio before the sweep, freeze it during, recenter
  only in silence, and invalidate on any emergency correction.
  → [plan](docs/superpowers/plans/2026-06-16-d6-frozen-src-ratio.md)
- **Full clip statistics (R4 / R8)** — the remaining per-session stats the first slice left out:
  positive-vs-negative rail split, first/last clip position, clipped-sample percentage, and a numeric
  per-channel peak / remaining-headroom readout.
  → [plan](docs/superpowers/plans/2026-06-16-clip-statistics.md)

### Amber — important reliability (not strictly blocking)

- **Block non-Auto combine modes for Dirac (D7)** — Sum / Average / Left / Right can still be recorded
  into a Dirac measurement (the Start gate ignores combine mode; the engine default is even `LeftOnly`).
  Gate or hard-confirm them with a real EARS + cable; default the engine to AutoPerEar.
  → [plan](docs/superpowers/plans/2026-06-16-d7-combine-mode-gating.md)
- **Re-validate device format mid-run (D8)** — sample rate / bit depth / channel count are latched once
  at start; a sleep/wake or shared-mode renegotiation mistunes the ASRC and can still read clean.
  Re-read per render block (or subscribe to a change notification) and invalidate on any change.
  → [plan](docs/superpowers/plans/2026-06-16-d8-runtime-format-revalidation.md)
- **Pre-clamp output-clip detection (D9)** — the output peak is measured *after* the ±1 safety clamp,
  so it can only ever flag the app's own clamp, not a real overload. Measure the pre-clamp peak.
  → [plan](docs/superpowers/plans/2026-06-16-d9-preclamp-output-clip.md)
- **Tests on the real audio path (R22)** — drive the actual `AudioIODeviceCallback`s in tests (today
  only a production-equivalent seam is exercised) plus a golden, sample-accurate cross-clock transport
  test, and the audit's device-reconnection / format-mismatch / drift / soak cases. *(The macOS
  aggregate path also can't currently detect FIFO-starvation or drift — it reports a nominal ratio —
  though that path is still inspection-only.)*
  → [plan](docs/superpowers/plans/2026-06-16-r22-audio-path-tests.md)

### Optional — diagnostics

- **Event capture buffer** — a pre-allocated circular buffer of recent *raw* EARS audio, dumped (with
  peaks, rail positions, timestamps, and device format) when a clip or corruption fires, for
  after-the-fact diagnosis. No blocking disk I/O on the audio thread.
  → [plan](docs/superpowers/plans/2026-06-16-diag-event-capture-buffer.md)
- **Possible-analog-saturation heuristics** — flat-top runs, crest-factor collapse, asymmetry —
  surfaced strictly as *possible saturation*, **never** labeled confirmed clipping (the input stayed
  below digital full scale).
  → [plan](docs/superpowers/plans/2026-06-16-diag-saturation-heuristics.md)
- **Callback-timing continuity** — use the audio callback's `hostTime` (currently ignored) to
  cross-check for clock glitches the FIFO-drift path can miss.
  → [plan](docs/superpowers/plans/2026-06-16-diag-hosttime-continuity.md)

Each item gets its own spec + plan before it's built; the rough order for the blockers is D2 → D5 → D6.

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

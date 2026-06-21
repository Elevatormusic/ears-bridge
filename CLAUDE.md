# EARS Bridge — working agreement

EARS Bridge bridges a miniDSP EARS / EARS Pro headphone-measurement jig to Dirac Live (per-ear calibration FIR → combine to one channel → virtual audio cable → Dirac records it as one mic). JUCE 8 / C++20, Windows + macOS. See `README.md` for the product, the private `docs/` plans for the roadmap.

This is **measurement software**: a bug that silently corrupts a measurement is worse than a crash, because the user trusts the result. Verification is held to that bar.

## Proactive gap-finding (the most important rule)

The user relies on me to surface what they **didn't** ask for — missing requirements, unhandled failure modes, and edge cases they don't know to specify. "Nobody wrote it down" is not an excuse; finding the unwritten requirement is the job.

- Before building any measurement / signal-chain feature, **enumerate failure modes first**: for each component in the chain (EARS input → calibration → combine → virtual cable → Dirac), list what can be misconfigured or go wrong (wrong rate, wrong channels, wrong bit-depth, shared/exclusive, swapped/mismatched cal, too much noise, clipping, dropout, drift, device loss), and check whether the change handles or at least detects each. Surface the gaps to the user as questions, not silent assumptions.
- Use the **`measurement-gap-finder`** subagent (`.claude/agents/`) for this enumeration on any non-trivial measurement feature — before building (to find missing requirements) and after (to find what the happy-path tests miss).
- When the user describes a feature narrowly, restate the failure modes you see and ask which to cover. Call out the ones they missed explicitly.

## Verification rules (mandatory — these exist because real bugs shipped without them)

1. **Negative tests for every detector and gate.** For each "X triggers it," write a test that "not-X does *not*." A detector with only happy-path tests is untested — e.g. a sweep-arm that's only tested to *arm* on a sweep, never tested to *reject* steady background noise, will false-fire in the field.
2. **GUI changes get a real render + screenshot check, not just a compile.** Unit tests and code review cannot see the rendered window — a buried message or a clipped label passes every test. Launch the built exe, capture it with the `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)` technique (the app is single-instance — kill the running one first; crop out the cal-card serial), and confirm the message is actually visible and not truncated. Do this for any status/label/panel change.
3. **On-device ratification is a release gate, not a footnote.** Anything that depends on real hardware behavior (a real room's noise floor, a real Dirac sweep's timing, a real endpoint's format) cannot be validated headless. Keep an explicit "needs on-device ratification" list; it must be cleared by the user (who has the EARS + Dirac) **before** that change ships to users in a release. Do not let it accumulate silently.
4. **Failure-mode enumeration before a measurement feature** (see Proactive gap-finding above) — so missing requirements surface as design questions, not field bugs.

## Build / test

- Build app: `./tools/dev.cmd cmake --build build --target EarsBridge`. Tests: `--target eb_tests`. Suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`. Run `tools/dev.cmd` DIRECTLY from Bash (it sets the MSVC env) — never bare `cmake`, never `cmd /c`. CI (`.github/workflows/ci.yml`) runs the suite on Windows + macOS for every push/PR; releases are gated on it.
- The repo lives inside OneDrive; the user should keep `build/` out of OneDrive sync (it caused a disk-full mid-merge once).

## Hard constraints

- **RT-safety:** the audio callback path (`ProcessingGraph::process`, the capture/render callbacks) must stay allocation/lock/syscall-free. No exceptions.
- **WASAPI shared mode echoes the requested sample rate** from `getCurrentSampleRate()` even while the OS resamples — never use it to detect the real rate; read the endpoint format blob (`platform/EndpointFormat`).
- **In-place `juce::dsp::FFT::perform` corrupts results** in this toolchain — always transform out-of-place. `juce::dsp::Convolution` loads IRs async with a gain ramp — let it settle before a measurement.
- **Never put the real EARS serial in a committed file** (tests/site/docs use the `000-0000` placeholder).
- **Commit attribution:** author as `Elevatormusic` (the no-reply identity is set); do **not** add a `Co-Authored-By` trailer. Stage specific files — never `git add -A` (private docs live under `docs/`, locally excluded).
- The repo is **public** and marked **alpha / pre-release**.

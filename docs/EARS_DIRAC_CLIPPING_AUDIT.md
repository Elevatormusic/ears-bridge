# EARS → Dirac Clipping & Stream-Integrity Audit

**Scope:** Can EARS Bridge truthfully report *"No digital clipping was detected in the raw audio received from the miniDSP EARS USB interface during the Dirac measurement sweep,"* and can it guarantee the forwarded sweep was not corrupted, dropped, duplicated, retimed, or mis-routed by the bridge itself?

**Method:** Read-only audit. Eight parallel dimensional auditors plus five adversarial verifiers traced actual executable control flow (not names/comments), with the load-bearing claims independently refutation-tested. Findings cite `file:line`. No production code was modified.

**Build context:** JUCE 8.0.4, Windows WASAPI **shared mode** (primary, validated) + macOS CoreAudio aggregate (inspection-only, unvalidated). Audio path compiled into `eb_engine` / `eb_gui` (`CMakeLists.txt:53-128`).

---

## 1. Executive verdict

**Status: Present but unreliable — *not safe to ship the required no-digital-clipping claim as worded.***

The bridge has a competent, real-time-safe **level/health monitor** and a genuinely-wired **buffer-integrity latch**. But it is a *calibration bridge with guidance meters*, not a *measurement-validity clipping gate*, and it cannot back the specific claim in the brief. Three findings each independently block that claim:

1. **It never sees the raw EARS digital stream.** The EARS is opened in WASAPI shared mode with the OS sample-rate converter armed (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), so the app observes Windows-audio-engine-processed float, not the device's integer PCM. "Exact rail hit = confirmed clip" is structurally impossible here. *(Claim D — refuted.)*
2. **A detected input clip does not invalidate the measurement.** `ClipInput` is guidance-only; it does not clear `cleanCapture`, so a clipped sweep still reports green **"Running · clean."** A test even asserts this. *(Claim B — refuted.)*
3. **There is no measurement session / sweep window.** Clipping/integrity is scoped to the user's Start→Stop *engine run*, not to Dirac's sweep, so pre- and post-sweep room events fold into the same result and the UI's *"overloading on the sweep"* timing claim is unjustified. *(Claim E — narrowed.)*

**The honest claim it *can* defend today:**
> *During the run, no sample in the forwarded stream reached the app's near-full-scale threshold (−1.0 dBFS for the warning flag, −0.009 dBFS for the meter), and no buffer dropout/overrun/sustained-drift/device-loss occurred on the Windows path.*

**The claim it *cannot* defend:**
> *No digital clipping was detected in the raw audio received from the EARS USB interface.* — It doesn't observe the raw audio, doesn't detect rails, doesn't fail the measurement on a clip, and (NaN/Inf, runtime-format-change) can pass some corrupted captures as clean.

Per the brief's release-gate rule (clipping detection **and** continuity are equal blockers, and *"if this cannot be conclusively verified → not safe to ship"*): continuity is solid for the FIFO/dropout class but has confirmed holes (NaN/Inf unchecked, runtime format change unvalidated, the production callbacks are never exercised by tests, the resampler ratio is not frozen during the sweep). **Verdict: do not ship the no-digital-clipping claim; the underlying app is otherwise functional as a calibration bridge.**

---

## 2. Actual architecture found

```
 EARS (2-ch USB mic)
   │  WASAPI SHARED, AUTOCONVERTPCM + SRC_DEFAULT_QUALITY armed  ← OS may resample/mix
   │  (juce_WASAPI_windows.cpp:409-411, 836-838, 866-867; type "Windows Audio" = shared :1957)
   ▼
 DeviceManager::openInput  inDev->open(...)            DeviceManager.cpp:159-164  (no mode override anywhere in src/)
   │   float buffers, getCurrentBitDepth()/SampleRate negotiated by the driver  :170-192
   ▼
 ── CAPTURE AUDIO THREAD ──────────────────────────────────────────────────────
 CaptureCallback::audioDeviceIOCallbackWithContext     AudioEngine.cpp:27-49
   ├─ guard numIn<2 → reportXrun(); return            :31   (invalidates)
   ├─ pkL/pkR = max|in[0]|,|in[1]|  (RAW, pre-FIR)     :34-35
   ├─ reportInLevels(pkL,pkR, pk>=0.999f)             :36   ← INPUT CLIP DETECTION
   ├─ graph.process(l,r,mono,N)                       :38   ── ProcessingGraph ──
   │     ├─ copy l/r → scratch                        ProcessingGraph.cpp:84-85
   │     ├─ convL/convR (per-ear inverse-cal FIR)     :87-90  (juce::dsp::Convolution)
   │     ├─ combine: LeftOnly/RightOnly/Sum(+6dB)/Average/AutoPerEar  :97-139
   │     ├─ g = outGain(≤4) * headroomGain(≤1)        :146-147
   │     └─ clip(outMono, −1, +1)                     :152   (hard clamp; does NOT strip NaN)
   ├─ bridge.pushCapture(mono,N)                      :39   ── ClockBridge (FIFO) ──
   └─ dropped delta → reportDroppedFrames             :45-48  (overrun → invalidates)
 ─────────────────────────────────────────────────────────────────────────────
 ClockBridge: AbstractFifo + LagrangeInterpolator ASRC + PI fill-control   ClockBridge.cpp:54-103
   ratio republished EVERY render block (no sweep-freeze)                  :71-80
   ▼
 ── RENDER AUDIO THREAD (master clock) ────────────────────────────────────────
 RenderCallback::audioDeviceIOCallbackWithContext     AudioEngine.cpp:64-90
   ├─ got = bridge.pullRender(mono,N)  (zero-pads on starvation)  :69
   ├─ pk = max|mono|; reportOutLevel(pk, pk>=0.999f)  :70-72  ← OUTPUT CLIP (post-clamp)
   ├─ underrun edge → effGot=0 → observeRenderBlock   :77-84  (FifoStarved/Dropout/Drift → invalidate)
   └─ duplicate mono → out[ch] = mono (L = R)         :85-89
   ▼
 Virtual cable (VB-CABLE) → Dirac Live records it as its mic input
 ── GUI MESSAGE THREAD (30 Hz) ──  MainComponent::timerCallback  MainComponent.cpp:727-799
   reads atomic snapshots: levels()→meters; flags()/cleanCapture()→status line; consumeRecentInputClip()→hint
```

| Aspect | Reality | Evidence |
|---|---|---|
| Audio API | WASAPI shared (Win), CoreAudio aggregate (mac, unvalidated) | `DeviceManager.cpp:27-28`; `juce_WASAPI_windows.cpp:1957` |
| Sample format seen by app | **float** (driver-converted; OS SRC armed) | `DeviceManager.cpp:170-176`; `juce_WASAPI_windows.cpp:836-838` |
| Threads | capture, render (master), optional verify, GUI 30 Hz | `AudioEngine.cpp:13,53,96`; `MainComponent.cpp:299` |
| Forwarded signal | per-ear inverse-cal FIR → combine → makeup → trim → clamp → mono L=R | `ProcessingGraph.cpp:84-152`; `AudioEngine.cpp:85-89` |
| Clock bridge | AbstractFifo + Lagrange ASRC + PI fill (ratio varies every block) | `ClockBridge.cpp:54-103` |
| Canonical "valid" signal | `HealthMonitor::cleanCapture` (latches false on Xrun/Dropout/ExcessDrift/FifoStarved **only**) | `HealthMonitor.cpp:8-18` |

---

## 3. Requirement matrix

| # | Requirement | Status | Evidence | Risk | Required action |
|---|---|---|---|---|---|
| R1 | Clip detected on **raw, pre-FIR/pre-combine** input, per channel | Partially implemented | `AudioEngine.cpp:34-36` peaks `in[0]/in[1]` before `graph.process` (:38); per-channel flags `cL/cR` | Low — placement correct | Keep; fix threshold (R3) |
| R2 | See **exact integer PCM rails** (≥1 rail = confirmed clip) | **Not implemented** | shared-mode float + `AUTOCONVERTPCM` (`juce_WASAPI_windows.cpp:836-838`); float carries no integer code | High — cannot detect a true rail; can't make the claim | Pin device format / detect consecutive ≈full-scale runs; see §7 |
| R3 | Single consistent clip threshold | Implemented incorrectly | meter `0.999f` (`AudioEngine.cpp:36,72`) vs flag `kClipLinear=0.8913f` (`HealthMonitor.h:29`, used `:70,91`) — ~1 dB apart | High — meter & warning disagree | One shared constant |
| R4 | Track clip stats (rail count, runs, longest, first/last ts, %, ±rail, session peak, one-hit vs sustained) | **Not implemented** | `HealthMonitor.h:69-80` holds only current bool + last-block peak; `std::abs` drops sign (`AudioEngine.cpp:35`) | High — can't grade severity or localize | Add per-session clip stats struct |
| R5 | A confirmed input clip **invalidates** the measurement | **Not implemented** | `ClipInput` excluded from invalidating set (`HealthMonitor.cpp:11-17,71`); test asserts `cleanCapture()` stays true (`test_healthmonitor.cpp:77-78`) | **Critical for the claim** — clipped sweep reads "clean" | Make a *confirmed* clip latch an invalid-measurement state (session-scoped) |
| R6 | Clip result exposed outside the callback safely | Fully implemented | all atomics; `recentInputClip()` edge-exchange (`HealthMonitor.h:53`) | None | — |
| R7 | Reset clip state between sessions | Fully implemented | `start()→prepare()→reset()` (`AudioEngine.cpp:294`, `HealthMonitor.cpp:29-39`) | None | — |
| R8 | Input headroom: per-ch dBFS, remaining headroom, near-FS vs confirmed-clip separated | Partially implemented | visual meter only (`LevelMeter.cpp:94-117`); no numeric headroom; near-FS == clip | Medium — no objective headroom | Expose per-ch peak dBFS + headroom |
| R9 | Capture overrun / render underrun / sustained drift invalidate (not just counted) | Fully implemented | `reportDroppedFrames→Dropout`; underrun edge→`FifoStarved`; drift state machine→`ExcessDrift`; all latch `cleanCapture=false` (`HealthMonitor.cpp:46-55,116-137`) | None (Windows path) | — |
| R10 | USB disconnect / device loss → Error, not false "clean" | Fully implemented | `audioDeviceStopped→deviceDied_` → `onDeviceLost→Error` (`AudioEngine.cpp:25,62,179-187`; `MainComponent.cpp:731-736`) | None | — |
| R11 | NaN/Inf in float samples detected/sanitized | **Not implemented** | zero `isfinite` guards in audio path; `clip()` passes NaN through (`ProcessingGraph.cpp:152`); peak meter doesn't trip on NaN | High — corrupt capture reads "clean" | `isfinite` scan → invalidate |
| R12 | Runtime format change (rate/depth/channels) after start detected | **Not implemented** | rate latched once (`AudioEngine.cpp:60,273`); never re-read per block; downgrade checked once at start (`MainComponent.cpp:576-584`) | Medium — mistuned ASRC can read clean | Re-validate per block / on notification |
| R13 | Both EARS channels monitored even when one is forwarded | Fully implemented | `reportInLevels(pkL,pkR,...)` always (`AudioEngine.cpp:34-36`); both meters | None | — |
| R14 | Forwarding is the intended transform (cal FIR + combine), channel identity correct | Fully implemented | `ProcessingGraph.cpp:28-37,87-90`; L cal→`in[0]`, R cal→`in[1]`; `LrVerify` wiring self-check | None | — (it is *not* unity by design) |
| R15 | Unity/gain tested **numerically** | Fully implemented | `test_processinggraph.cpp:77-219` (combine ratios, gain, clamp, headroom balance, clearFir passthrough) | None | — |
| R16 | Peak/clip at **both** raw input and final cable output | Fully implemented (with caveat) | input `:34-36`; output `:70-72` (but post-clamp) | Low | note output clip is post-clamp |
| R17 | Non-Auto combine modes blocked for Dirac (no hidden summing) | Implemented incorrectly | Start gate ignores combine mode (`MainComponent.cpp:592-600`); Sum/Average/Left/Right only flagged via transient hint text (`:503-513`); engine default = `LeftOnly` (`ProcessingGraph.h:32`) | Medium — user can record Sum/Average into Dirac | Gate or block non-Auto for a real EARS+cable; default AutoPerEar |
| R18 | Explicit measurement **session** (idle/preflight/sweep/postroll/complete/invalid) | **Not implemented** | only `EngineStatus{Stopped,Running,Error}` (`EngineTypes.h:6`); no sweep detection/pre/post-roll | High — can't scope result to the sweep | Add a session state machine |
| R19 | Stable SRC ratio held during the sweep; recenter only in silence; per-sweep invalidation | **Not implemented** | ratio republished every render block (`ClockBridge.cpp:71-80`); no sweep awareness | High — continuous ratio creep smears the log-sweep IR below the drift tolerance → reads clean | Freeze ratio during measurement |
| R20 | UI wording is technically justified (no "no-clip-anywhere"; no near-rail = clip; no unjustified "on the sweep") | Implemented incorrectly | "Running · clean" over a latched `ClipInput` (`MainComponent.cpp:671`); "the EARS is overloading on the sweep" at −1 dBFS float with no sweep detection (`:241-243`) | Medium/High — misleads the user both ways | Reword per §6 of the brief |
| R21 | Audio callbacks RT-safe (no alloc/lock/IO/FFT/log) | Fully implemented | three callbacks + timer clean; FIR/Convolution off-thread (`MainComponent.cpp:528-557`) | None (two cosmetic caveats) | — |
| R22 | Tests exercise the **production** path | Implemented incorrectly | callbacks never instantiated; clip tested only on helper/0.8913 path, never the `0.999` callback path; RT-safety asserted by comment only | High (confidence) — green tests don't cover the shipped path | Add callback-level + golden-transport tests |

---

## 4. Confirmed defects

### D1 — Input clip never invalidates the measurement *(Critical for the claim)*
- **Where:** `HealthMonitor::raise` (`HealthMonitor.cpp:8-18`), `reportInLevels` (`:65-73`); consumed at `MainComponent::updateStatusLine` (`:646-690`).
- **Now:** `ClipInput` is raised but excluded from the invalidating set `{Xrun,Dropout,ExcessDrift,FifoStarved}`. `cleanCapture` stays `true`; the status line shows green **"Running · clean."** The sticky `ClipInput` flag is *read nowhere* in the status logic (`updateStatusLine` only checks `ClipOutput`). `test_healthmonitor.cpp:77-78` asserts this is intended.
- **Why wrong / impact:** Directly violates the brief's validity rule ("≥1 confirmed clip = invalid measurement"). A clipped Dirac sweep is reported clean; the user calibrates against a clipped capture.
- **Minimum fix:** When a *confirmed* clip occurs inside the measurement window, latch a distinct `MeasurementClipped`/invalid state (do **not** fold it into the dropout "Dropouts detected" path — keep the classes separate per the brief).

### D2 — App never observes the raw EARS digital stream *(High; structural)*
- **Where:** `DeviceManager.cpp:27-28,159-164`; JUCE `juce_WASAPI_windows.cpp:409-411,836-838,866-867`.
- **Now:** Device type is `"Windows Audio"` = `WASAPIDeviceMode::shared`; JUCE arms `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` and `Initialize(AUDCLNT_SHAREMODE_SHARED, …)`. The app's own code notes "WASAPI shared mode can grant a different rate/depth" (`MainComponent.cpp:573`).
- **Why wrong / impact:** The clip detector sees the Windows-audio-engine output (post int→float, post-mixer level, and **post-SRC whenever the requested rate ≠ the endpoint mix format**), not the device's integer rails. "Exact rail hit" is unobtainable; SRC can move inter-sample peaks; the shared mixer's endpoint volume scales level. *Nuance:* in the matched-rate case a true full-scale integer maps to ±1.0f and is detectable in scale, so the path is not worthless — but it cannot *guarantee* raw-rail fidelity, which is what the claim requires.
- **Minimum fix:** Pin the EARS to its native rate/depth and verify `getCurrentSampleRate()==requested` (so no SRC engages) and document the residual shared-mixer caveat; or open the EARS WASAPI-exclusive for measurement. Then redefine "confirmed clip" as a run of consecutive samples at/above a near-rail ceiling, not a single peak.

### D3 — Two inconsistent clip thresholds *(High)*
- **Where:** `AudioEngine.cpp:36,72` (`0.999f`, −0.009 dBFS) vs `HealthMonitor.h:29` `kClipLinear=0.8913f` (−1.0 dBFS) used at `HealthMonitor.cpp:70,91`.
- **Now:** The per-channel meter "CLIP" (from `Levels.clipL/clipR`) fires at −0.009 dBFS; the status-bar warning + sticky flag fire at −1.0 dBFS. A −0.7 dBFS peak warns in the status bar but shows no meter CLIP.
- **Impact:** Meter and warning contradict each other for the same block; neither is "clipping" in any defensible sense.
- **Minimum fix:** One named constant for the whole path; choose its meaning deliberately (near-rail warning vs confirmed clip — they should be *different, explicitly labeled* states, not one boolean at two values).

### D4 — No NaN/Inf guard anywhere in the audio path *(High)*
- **Where:** capture/render callbacks (`AudioEngine.cpp:27-90`), `ProcessingGraph::process` (`:80-153`).
- **Now:** No `isfinite` check exists. `FloatVectorOperations::clip` (`:152`) does min/max, which passes NaN through (NaN compares false to both bounds). The `std::abs`+`jmax` peak meters propagate NaN into a peak that never trips `>=0.999f`.
- **Impact:** A single non-finite sample (convolution denormal blowup, driver glitch) corrupts the Dirac recording while `cleanCapture` stays true and the UI shows "Running · clean."
- **Minimum fix:** Scan the processed mono block (and ideally the raw input) for non-finite samples; on detection, zero the block **and** latch an invalidating flag.

### D5 — No measurement session / sweep scoping *(High)*
- **Where:** `EngineTypes.h:6` (`EngineStatus{Stopped,Running,Error}`); `HealthMonitor.h:11-84`; `MainComponent.cpp:559` (only Start/Stop boundaries).
- **Now:** Stats reset per *engine run*, not per *sweep*. There is no sweep detection, pre-roll, or post-roll. Pre-sweep noise latches `ClipInput`/`cleanCapture=false`; post-sweep noise can retroactively brand a clean sweep failed/clipped.
- **Impact:** The result cannot be scoped to "during the Dirac sweep," which the claim explicitly requires.
- **Minimum fix:** Add a session state machine (Idle→Preflight→SweepActive→Complete/Invalid) with a level-threshold or explicit-action sweep boundary, and scope clip/integrity latching to the active window.

### D6 — Clock-bridge SRC ratio is never frozen during the sweep *(High)*
- **Where:** `ClockBridge.cpp:71-80`; drift watch `HealthMonitor.cpp:128-137`.
- **Now:** The PI fill-controller republishes a new resample ratio every render block, continuously, with no sweep awareness. Sustained drift only latches above `kDriftRatioTol=0.005` (±0.5%).
- **Impact:** Continuous sub-0.5% ratio creep nonuniformly retimes Dirac's log sweep — smearing the recovered impulse-response phase — while staying *under* the drift threshold, so it reports "clean." This is exactly the "nonuniformly retime samples during a sweep" failure the brief calls a release blocker.
- **Minimum fix:** Estimate the ratio before the sweep, **freeze** it during the active window (absorb short-term drift with the existing FIFO headroom), recenter only in silence, and invalidate if an emergency correction is forced mid-sweep.

### D7 — Non-Auto combine modes can reach Dirac *(Medium)*
- **Where:** `MainComponent::updateStartGate` (`:592-600`) ignores combine mode; hints only (`:503-513`); engine default `LeftOnly` (`ProcessingGraph.h:32`, `CombineMode.h:6`).
- **Now:** Start is gated only on devices + both cals. Sum (+6 dB, uncompensated), Average (folds open-back crosstalk), Left/Right are blocked by *prose only.* A first-run/settings-reset engine defaults to `LeftOnly`.
- **Impact:** A user can record a summed/single-ear signal into a Dirac measurement; the brief treats hidden/again-enabled summing as a major defect.
- **Minimum fix:** Gate/confirm non-AutoPerEar with a real EARS+cable; default the engine to AutoPerEar.

### D8 — Runtime format change not revalidated *(Medium)*
- **Where:** `AudioEngine.cpp:60,273` (rate latched once); no per-block check.
- **Impact:** A mid-run shared-mode renegotiation (sleep/wake, OS format change) mistunes the ASRC nominal ratio; the only symptom is drift that may stay under tolerance → reads clean.
- **Minimum fix:** Re-read `getCurrentSampleRate/BitDepth/activeInputChannels` per render block (or subscribe to a change notification) and invalidate on any change vs the prepared values.

### D9 — Output clip detector runs after the app's own clamp *(Medium)*
- **Where:** `AudioEngine.cpp:70-72` reads mono **after** `ProcessingGraph.cpp:152` clamped it to ±1.
- **Impact:** The "output clip" can only ever flag the app's own clamp (e.g., Sum +6 dB), not a converter overload, and shares D3's threshold split. Acknowledged in `MainComponent.cpp:654-656`.
- **Minimum fix:** Measure the pre-clamp peak for the warning; keep the clamp as a safety net.

### D10 — Misleading UI wording *(Medium/High)*
- **Where:** `MainComponent.cpp:671` ("Running · clean"), `:241-243` ("Input clipping — the EARS is overloading on the sweep").
- **Impact:** "clean" reads as an all-clear about signal integrity though it means only "no dropouts this run." "overloading on the sweep" asserts analog ADC saturation (unproven at −1 dBFS float) and sweep timing (no sweep detection exists). One boolean conflates confirmed-digital-clip / possible-analog-saturation / no-clip / corruption.
- **Minimum fix:** Reword to the brief's §12 phrasings; separate the four states.

---

## 5. False-confidence risks

- **Green "Running · clean" is not a no-clipping all-clear.** It reflects only the four dropout-class flags; a latched `ClipInput` does not disturb it (`HealthMonitor.cpp:71`). *(D1)*
- **A clip "flag" that is never consumed.** The sticky `HealthFlag::ClipInput` is read nowhere in the status logic; only the separate self-clearing hint surfaces input clips, and only transiently. *(D1)*
- **"CLIP" / "overloading" on near-rail, not clipping.** A clean −0.5 dBFS sweep prints "CLIP" and "overloading," nudging the user to *lower* gain and *hurt* SNR — the exact failure the low-level guidance was meant to prevent. *(D3, D10)*
- **Clipping detection on post-processed samples.** The "raw" samples are OS-mixed/SRC float, not device rails. *(D2)*
- **Tests that don't exercise the production path.** `processCaptureBlockForTest` (`AudioEngine.cpp:351-354`) runs `graph.process` only; the three `AudioIODeviceCallback`s are never instantiated; the overrun→Health wiring is re-implemented inside `test_clockbridge.cpp:193-199` rather than driven through the real callback; the `0.999f` capture-clip path is never run; RT-safety is asserted by comment, not by a harness. Green suite ≠ covered shipped path. *(D-test)*
- **macOS path silently can't detect two failure classes.** On the aggregate path `observeRenderBlock` is hardcoded to report no shortfall + nominal ratio (`AudioEngine.cpp:83`), so `FifoStarved`/`ExcessDrift` cannot latch there — and macOS is inspection-only/unvalidated.
- **NaN/Inf passes as clean.** No finite-check; clamp doesn't strip NaN. *(D4)*

---

## 6. Missing implementation

**Must fix before claiming reliable clipping detection**
- Make a *confirmed* input clip latch an **invalid measurement** (D1, D5).
- Define "confirmed clip" honestly given shared-mode float: pin native rate (no SRC) + detect a **run of consecutive near-rail samples**, not a single peak; or open exclusive (D2, D3).
- One clip-threshold constant; separate "near-rail warning" from "confirmed clip" as distinct states (D3).
- NaN/Inf scan → invalidate (D4, R11).
- Per-session clip statistics: count, longest run, first/last position, ±rail, clipped-% , session peak/headroom (R4, R8).

**Important reliability improvements**
- Session state machine + sweep scoping (D5).
- Freeze SRC ratio during the sweep; invalidate on emergency correction (D6).
- Re-validate device format per block (D8).
- Gate/block non-Auto combine modes for Dirac; default AutoPerEar (D7).
- Measure pre-clamp output peak (D9).
- Reword UI to the brief's §12 (D10).
- Tests that drive the real callbacks + a golden sample-accurate transport test (R22).

**Optional diagnostics**
- Pre-allocated circular raw-capture buffer dumped on a clip/corruption event (brief §11) — not currently present; belongs in a later phase.
- Analog-saturation heuristics (flat-top run, crest-factor collapse) — explicitly **possible-saturation**, never "clipping" (brief §4).
- hostTime continuity cross-check (`AudioIODeviceCallbackContext` is currently ignored, `AudioEngine.cpp:30,67,104`).

---

## 7. Recommended architecture (smallest change)

The current design is sound enough to **extend, not rewrite.** The monitor is already lock-free and correctly placed at the raw input; the gap is *semantics and scoping*, not plumbing.

1. **Honest "confirmed clip" definition.** In `DeviceManager::openInput`, request the EARS native rate and assert `getCurrentSampleRate()==requested` (so AUTOCONVERTPCM does not resample); store the granted depth. In the capture callback, replace the single-peak `>=0.999f` test with a small per-channel **consecutive-near-rail-run counter** (run length ≥ N samples at ≥ a single ceiling constant). One ceiling, one definition, both channels.
2. **Two explicit states, not one bool.** Keep the existing free-running meter/`recentInputClip` as the **near-rail warning**. Add a separate **confirmed-clip** signal that (a) carries stats (count, longest run, first/last index, per-channel) and (b) latches an **invalid-measurement** flag — a *new* member alongside `cleanCapture`, surfaced as its own status line, kept distinct from "Dropouts detected."
3. **Session scoping.** Add `MeasurementSession{Idle,Preflight,SweepActive,Complete,Invalid}` driven by a level-threshold sweep boundary (or an explicit "Arm measurement" button). Reset clip/integrity latches at `SweepActive` entry; freeze the ClockBridge ratio for its duration; ignore events outside it.
4. **Content-integrity guard.** One `isfinite` scan per processed block → invalidate. Cheap, RT-safe.
5. **Wording + gating.** Reword per §6; consult combine mode in `updateStartGate`.

No new threads, no new libraries, no change to the FIR/combine/clock design.

---

## 8. Implementation sequence (proposed — not yet implemented)

> Per the brief: this is the patch plan only. No code has been changed.

| Phase | Work | Files | Acceptance | Tests |
|---|---|---|---|---|
| **0. Instrumentation/tests first** | Stand up a harness that drives the *real* `CaptureCallback`/`RenderCallback` with synthetic buffers; assert no-alloc/no-lock | `tests/test_audioengine_callbacks.cpp` (new), `AudioEngine.h` (expose callbacks for test) | callbacks run in-test; RT-safety asserted | unit |
| **1. Confirmed raw-input clipping** | native-rate pin + no-SRC assert; consecutive-run clip detector; per-session stats; one ceiling constant | `DeviceManager.cpp`, `AudioEngine.cpp`, `HealthMonitor.{h,cpp}`, `EngineTypes.h` | exact +rail, −rail, isolated hit, sustained, clean −0.1/−1 dBFS, L-only/R-only/both all correct | unit + simulated-device |
| **2. Stream-integrity invalidation** | NaN/Inf scan→invalidate; per-block format re-validate; distinct `MeasurementInvalid` separate from dropout | `ProcessingGraph.cpp`, `AudioEngine.cpp`, `HealthMonitor.{h,cpp}` | NaN/Inf, rate/format change, channel drop each invalidate; latched until reset | unit + integration |
| **3. Clock-domain bridge validation** | freeze SRC ratio during `SweepActive`; invalidate on emergency correction; golden sample-accurate transport test across the two clock domains | `ClockBridge.{h,cpp}`, `AudioEngine.cpp` | ratio constant during sweep; injected drift invalidates only on real correction; transport bit-exact within ASRC tolerance | integration + soak |
| **4. Session state + UI** | session machine + sweep boundary; reword status/clip hints per §12; gate non-Auto combine | `MainComponent.{h,cpp}`, `EngineTypes.h` | pre/post-sweep events excluded; wording matches §12; Sum/Average blocked/confirmed for Dirac | unit + manual |
| **5. Optional diagnostics + saturation** | circular raw buffer on event; flat-top/crest heuristics labeled *possible saturation* only | new `DiagnosticCapture`, `HealthMonitor` | event buffer retained; saturation never labeled "clipping" | unit |

---

## 9. Test plan

**Unit (pure, no device):** exact +rail / −rail / isolated single hit / sustained run / clean −0.1 dBFS / clean −1 dBFS (must *not* read confirmed-clip) / one-hit-vs-sustained classification / per-channel L-only, R-only, both / ±rail sign preserved / session-reset clears stats / clip flag → invalid-measurement latch.

**Integration (real callbacks, synthetic buffers — the gap today):** drive `CaptureCallback`+`RenderCallback`; input-clean-but-output-clipped; gain accidentally > unity; ring-buffer overflow & underflow; NaN/Inf → invalidate; channel-drop → invalidate; clip *before / during / after* the sweep window scoped correctly; latched-invalid UI state.

**Simulated-device:** sample-rate mismatch (SRC engaged) shifts peak vs native-rate baseline; bit-depth downgrade; format change mid-run; device reconnection; integer→float boundary (full-scale int → ±1.0f detected).

**Clock/transport:** sample-accurate golden transport across EARS↔cable clock domains; injected ±0.5–3% drift — frozen ratio holds during sweep, emergency correction invalidates; long soak (no creep-induced retiming passes as clean).

**Hardware-in-the-loop:** real EARS + Dirac sweep with a deliberately over-driven level (confirm invalidation), a borderline −1 dBFS clean level (confirm *no* false clip), and L-only / R-only physical overload.

---

## 10. Final recommendation

- **Safe to ship?** **Not for the specific no-digital-clipping claim.** The bridge is a solid *calibration forwarder* with real-time-safe guidance meters and a genuine dropout latch (Windows path), and it is fine to ship *as that.* It is **not** a measurement-validity clipping gate and must not be presented as one.
- **Is the current clipping claim technically defensible?** **No.** It detects "near-full-scale on an OS-processed float stream" and does not fail the measurement on a clip, does not see raw rails, has no sweep scope, and can pass NaN/format-change corruption as clean.
- **Smallest set of release blockers** (before *any* no-clipping claim): D1 (clip must invalidate), D2/D3 (honest confirmed-clip definition + native-rate pin + single constant), D4 (NaN/Inf), D5 (sweep scoping), D6 (frozen SRC ratio during sweep), and Phase-0/1 tests that exercise the real callbacks (R22). D7–D10 are strongly recommended alongside.
- **Explicitly out of scope** (keep it that way): generalized headphone-distortion analysis; proving the analog chain (DAC/amp/headphone/EARS preamp) didn't saturate — the app may only ever claim *digital* observations, and any flat-top/crest heuristic must be labeled *possible analog saturation*, never "clipping."

**Honest claim the app should make instead, today:**
> *"No near-full-scale level and no buffer dropout were detected in the forwarded stream during this run."* — true, defensible, and clearly **not** "no clipping anywhere in the chain."

---

*Report location:* `docs/EARS_DIRAC_CLIPPING_AUDIT.md` (the repo has no prior audit convention; `docs/` already holds the bench-validation runbook and the superpowers specs/plans, so this sits alongside them). No production code was modified; §7–§8 are a proposed patch plan only.

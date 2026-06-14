# EARS Bridge — Design Spec

**Date:** 2026-06-13
**Status:** Draft for review
**Working name:** EARS Bridge

---

## 1. Purpose & context

The [miniDSP EARS](https://www.minidsp.com/products/acoustic-measurement/ears-headphone-jig) and its successor the [EARS Pro](https://www.minidsp.com/products/acoustic-measurement/ears-pro) are USB headphone-measurement jigs that present to the OS as a **2-channel capture device** (channel 0 = left-ear mic, channel 1 = right-ear mic). Each capsule has its own per-unit calibration file (FRD format: `freq(Hz)  SPL(dB)  phase(deg)`). The owner has the original-EARS compensation sets for serial **000-0000** — `L_HEQ`/`R_HEQ` and `L_HPN`/`R_HPN`. **The app supports both the original EARS and the EARS Pro** (see §3 for the device differences).

The Dirac Live Calibration Tool (DLCT) corrects headphones well, but its measurement path accepts only **one** recording device and only **one** mic-calibration curve. It has no per-channel (left-ear vs right-ear) calibration import. There is no way, inside Dirac, to apply two different cal curves to two mic channels and combine them.

**EARS Bridge** closes that gap. It is a small cross-platform (Windows + macOS) JUCE/C++ desktop app that sits in the live audio path during a Dirac measurement:

1. captures the EARS 2 channels,
2. applies a per-channel FIR built from each ear's cal file,
3. combines the two calibrated channels into a single mono stream,
4. renders that mono into a user-selected **virtual audio device**, whose capture side Dirac then selects as its "Recording device".

The user picks the virtual device; the app does not ship a driver. (On Windows that's VB-CABLE / VoiceMeeter; on macOS, BlackHole / Loopback / an Aggregate Device.)

This spec reflects an adversarial verification pass (see `docs/research/` references inline and §13). Several first-cut assumptions were overturned; those corrections are baked in below.

---

## 2. Goals & non-goals

### Goals
- Apply the correct per-ear calibration to the EARS capture in real time, transparently enough that Dirac records the same result REW would compute from the same cal files.
- Combine to a single mono stream Dirac can read, with selectable combine modes.
- Driver-agnostic routing: auto-detect any installed virtual output and let the user choose.
- Modern, legible GUI with cal-curve thumbnails, level/clip metering, and clear device pickers.
- A clean, glitch-free audio path with no *unintended* resampling, and explicit handling of the unavoidable cross-device clock boundary.
- Cross-platform: Windows and macOS from one JUCE codebase.
- Support **both the original EARS and the EARS Pro**, and **16-, 24-, and 32-bit** audio, negotiating each device's native sample rate and bit depth (EARS Pro adds 88.2/96/176.4/192 kHz and 32-bit capture; the internal path is float32).

### Non-goals (YAGNI)
- No built-in sweep generator, measurement capture, or live FR analyzer — Dirac does the measuring. (We render only a *static* thumbnail of each loaded cal curve.)
- No custom virtual-audio driver.
- No support for measurement fixtures outside the miniDSP EARS / EARS Pro family (other 2-channel USB jigs with FRD cal files may work but are untested).
- No correction *target* shaping — that's Dirac's job. The Bridge does mic-only correction.

---

## 3. The corrected technical reality (drives the architecture)

These are the verified facts that shaped the design. Citations in §13.

1. **Two supported devices with very different capture capabilities.**
   - **Original EARS** — UAC1, driverless, **fixed 24-bit / 48 kHz**, 2-channel, 0–36 dB DIP gain. Cannot capture at any other rate; a non-48 kHz selection only resamples it.
   - **EARS Pro** — XMOS, **UAC2, 16/24/32-bit, 44.1–192 kHz**, 2-channel, 0–45 dB DIP gain, USB-C. Class-compliant (plug-and-play) on macOS/Linux and on Windows 10+ via WASAPI; a vendor **ASIO driver** is also provided but is **not used here** (ASIO can't bridge to a different output — item 3). Both devices share the IEC 60318-4 coupler and per-channel FRD `.txt` cal files, so the same `CalFile` / `FirDesigner` / `ProcessingGraph` core (already built rate-agnostic) handles both.
   The app must **detect which device is connected and expose only the sample rates and bit depths that device natively supports** (full 44.1–192 kHz, 16/24-bit for EARS Pro; 48 kHz, 24-bit for the original EARS), warning only when a chosen rate would force a resample of the selected input.

2. **Two independent clock domains.** Capturing from the EARS while rendering to a *different* device (a virtual cable) means two free-running clocks (EARS USB clock vs virtual-device clock) that **drift** over a multi-minute measurement, even when both are nominally 48 kHz. A lock-free FIFO plus an **asynchronous sample-rate converter (ASRC)** at the boundary is mandatory, not optional. "Bit-perfect, no resampling" is not achievable across two devices; the realistic guarantee is *in-band-transparent* SRC.

3. **ASIO cannot bridge two devices.** `AudioIODeviceType::hasSeparateInputsAndOutputs()` is `false` for ASIO. The EARS-in / virtual-out split must run on **WASAPI** (Windows) or **CoreAudio** (macOS).

4. **Dirac opens the recording device WASAPI-exclusive by default** (Dirac Live 3.11; opt-out env var `DAUDIO_WASAPI_NON_EXCLUSIVE=ON`). The Bridge must therefore write only to the virtual device's **render** side and never open its capture endpoint, leaving that capture side free for Dirac to hog.

5. **Dirac sweeps channels sequentially** and locates the impulse by log-sweep cross-correlation, so a fixed bridge+cable delay (tens of ms) merely shifts the impulse in the analysis window. Dirac's documented failure mode is *recording inaccuracy* (dropped buffers), which silently disables gain+delay compensation — so a Bridge xrun must be caught and surfaced by the Bridge, not blamed on Dirac.

6. **REW subtracts the cal curve** (`corrected = raw − cal`). The EARS cal files carry a large positive bump (~+13 to +22 dB near 4 kHz — the EARS canal/coupler resonance) that must be *removed* from the capture. So the Bridge's pre-filter applies the **inverse** of the cal curve (−dB). Exact polarity is validated by a REW round-trip (§13, open item).

7. **HPN ≠ HEQ.** HEQ bundles a *headphone target* (downslope + ~4.3 kHz resonance correction). Using HEQ as the mic-cal would bake a target that Dirac then targets *again* — a double-target bug. For the Dirac chain the Bridge defaults to **HPN** (mic-only compensation) and warns if HEQ is loaded as mic-cal.

---

## 4. Architecture

```
 miniDSP EARS (USB, 2ch, 24-bit/48 kHz)
   ch0 = Left ear ─┐
   ch1 = Right ear ┐│
                  ││
        WASAPI / CoreAudio capture callback (EARS clock domain)
                  ▼▼
        ┌───────────────────────────────────────────────┐
        │  Calibration / Processing graph                │
        │   ch0 → Convolution(FIR_left  = inverse cal L) │
        │   ch1 → Convolution(FIR_right = inverse cal R) │
        │   → Combine (TwoPass-L / TwoPass-R / Avg / Sum)│
        │   → mono                                       │
        └───────────────────────────────────────────────┘
                  ▼
        ┌───────────────────────────────────────────────┐
        │  Clock bridge: lock-free FIFO + ASRC           │
        │   (EARS clock → virtual-device clock)          │
        │   Windows: app-implemented FIFO+ASRC           │
        │   macOS:   prefer CoreAudio Aggregate Device   │
        │            (Drift Correction on, clock=EARS)   │
        └───────────────────────────────────────────────┘
                  ▼
        Render callback (virtual-device clock domain)
        mono duplicated → L = R  (both channels identical)
                  ▼
   Virtual output device (VB-CABLE / BlackHole / …) — RENDER side
                  ▼
   Dirac Live picks the virtual device's CAPTURE side as "Recording device"
   (WASAPI exclusive); applies its own target curve.
```

### Cross-device clocking — the key architectural addition
- **Windows:** there is no OS aggregation. Two `AudioIODevice` contexts run — the **virtual-device render callback is the master clock**; the EARS capture feeds a lock-free ring buffer (`juce::AbstractFifo`); an **ASRC** resamples capture→render with its ratio trimmed by a slow control loop driven by FIFO-fill level (PI on a smoothed fill estimate, **not** raw callback timestamps, to avoid "breathing"/periodic glitches).
- **macOS:** **prefer** creating/using a CoreAudio **Aggregate Device** (EARS + virtual device) with *Drift Correction ON* and *Clock Source = EARS*, so the OS handles resampling. Do **not** rely on JUCE's internal `AudioIODeviceCombiner` — it does no drift correction and is documented to glitch, accumulate latency, then go silent over 8–50 min. If an aggregate can't be created, fall back to the same app-implemented FIFO+ASRC as Windows.
- The ASRC must be **in-band transparent** (well above 20 kHz stopband at 48 kHz it's trivial). Magnitude-only min-phase measurement tolerates the tiny time-varying correction; flag a re-check if the optional phase-accurate FIR mode is used (§7).

---

## 5. Component breakdown

### 5.1 Audio device layer
- Enumerate input devices; identify the EARS / EARS Pro by a **stable composite key** (device name + USB VID:PID / serial), not a transient OS index, so re-plugs don't mis-bind cal files. Tag the recognised **model** (original EARS vs EARS Pro) so the UI exposes the correct native rates/bit-depths.
- For the selected input, **query its actually-supported sample rates and bit depths** and expose only those as native: original EARS → 48 kHz / 24-bit; EARS Pro → 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz and 16/24/32-bit. The chain runs **float32 internally** regardless of device bit depth.
- Enumerate output devices; tag likely virtual sinks (VB-CABLE, VoiceMeeter, BlackHole, Loopback) for a "recommended" affordance, but allow any. Let the user pick the **output bit depth (16 / 24 / 32-bit)** for the virtual sink (24-bit is the safe default — ample for measurement; some virtual cables / Dirac setups reject 32-bit while others like BlackHole are 32-bit-float native); request it on the render side and fall back to the nearest supported format.
- Open the virtual sink in **WASAPI shared** (Windows) so Dirac can later hog the capture side; never open the virtual capture endpoint.
- Use **WASAPI even for EARS Pro** — its vendor ASIO driver can't pair a capture device with a *different* render device. Detect ASIO-only configs (`hasSeparateInputsAndOutputs()==false`) and fall back to WASAPI with a clear message.

### 5.2 Calibration engine (off audio thread)
- **FRD parser:** tolerant of the miniDSP header (quoted lines, `*` comments, "Sens Factor", serial, version). Validates monotonic increasing frequency, parses `freq, SPL, phase`. Surfaces parse errors with line context. Reads the embedded **type tag** ("HEQ"/"HPN"/"RAW"/"IDF") for guidance/warnings.
- **FIR designer** (see §7 for math): builds the per-channel impulse response; runs on a worker thread; hands off via lock-free swap (`Convolution::loadImpulseResponse()`). Re-runs on file load or rate change.
- **Polarity:** pre-filter = inverse (`−SPL` dB) of the loaded curve (validated by REW round-trip).

### 5.3 Processing graph (audio thread, allocation-free)
- Two `juce::dsp::Convolution` instances (L, R), prepared at the active rate/block size.
- **Combine** stage (per §6): TwoPass-Left, TwoPass-Right, Average `(L+R)/2`, Sum `L+R`. FIR is applied **before** combine.
- Output is mono; written identically to both channels of the (stereo) virtual sink.

### 5.4 Routing / mono delivery
- **Duplicate mono to L and R** of the virtual device so the result is independent of whether Dirac reads ch0, ch1, sums, or averages.
- **Preflight checks** (Windows especially): warn if the virtual device is set as the Windows *Communications* default or has mic *enhancements*/noise-suppression enabled (identical L=R is pure common-mode content, which such DSP suppresses, especially in the bass). Warn if another app holds the virtual capture endpoint (Dirac's exclusive open would otherwise fail and read as "Dirac can't see my mic").
- Conservative output level so Dirac's meter lands ~−20 to −30 dB; user output trim; no internal clip stage.

### 5.5 GUI (modern, dark; see §8)

### 5.6 Health monitoring
- Count xruns/underruns and track accumulated capture↔render **slip** (FIFO-fill trend, dropped/silence-filled block counters) during a sweep; invalidate/warn on a measurement if a dropout or excessive drift occurs.
- Live input/clip meters (analog gain is a hardware DIP switch outside software control — 0–36 dB on the original EARS, 0–45 dB on EARS Pro — so warn on near-full-scale or very low capture).
- One-time **physical L/R verification** step (signal into one earcup) so a silent channel swap can't apply the wrong cal to each ear.

### 5.7 Settings persistence
- Remember device selections (by stable key), loaded cal files, combine mode, rate, output trim, advanced toggles.

---

## 6. Combine methodology & recommended workflow

**Recommended (headline) workflow — two-pass single-ear:** run Dirac **twice** — once in **TwoPass-Left** (only the left-ear cal/channel feeds mono), once in **TwoPass-Right**. This mirrors and improves miniDSP's official EARS+Dirac single-side method, eliminates crosstalk by construction, and gives each ear its exact correction. Each Dirac project corrects one headphone channel.

> The Bridge's Left/Right selection picks the **mic channel**. Full crosstalk immunity *also* requires the user to route Dirac's **playback** to a single earcup (a user/hardware action, exactly as the official method instructs). The app documents this explicitly.

**Quick alternative — Average `(L+R)/2`:** labeled "quick; matched, *sealed* closed-backs only", with an in-app warning that (a) Average blends two physically different ear responses into one curve matching *neither* ear, and (b) on open-backs, passive isolation collapses below ~200 Hz so the "silent ear contributes nothing" assumption fails in the bass.

**Sum `L+R`:** offered last; no methodological benefit; flagged for +6 dB level / clip risk.

UI orders modes by rigor: TwoPass single-ear → Average → Sum.

---

## 7. DSP detail — FIR design

**Default mode: magnitude-exact, minimum-phase FIR at the active sample rate** (48 kHz for the original EARS; up to 192 kHz for EARS Pro).

Pipeline:
1. Parse FRD → (freq, SPL dB, phase deg).
2. **Log-frequency interpolate** the magnitude onto a dense **linear** FFT grid.
3. Form the target as the **inverse** (`−dB`) of the cal curve (polarity validated, §13). Endpoint policy: below 10 Hz / above 20 kHz → hold/flatten, **clamp** boost to a safe ceiling.
4. IFFT → impulse; **window** with a Hann/Tukey taper (not rectangular).
5. Enforce **minimum phase** via real-cepstrum folding (discrete Hilbert of log-magnitude).

Sizing (the FIR is designed at the **active sample rate**, so taps scale with it to preserve low-frequency resolution):
- Baseline **N = 8192 taps @ 48 kHz** (resolution ≈ 5.9 Hz; LF-effective ≈ 18–20 Hz). Since resolution = fs/N, **scale taps with rate** — `N ≈ 8192 × (fs / 48 kHz)` rounded to a power of two (≈ 16384 @ 96 kHz, ≈ 32768 @ 192 kHz) — so EARS Pro's high rates keep the same bass reach. N = 4096 (lighter) and N = 16384 (finer) are also selectable in advanced settings.
- **Design-time FFT length 4–8× N** before windowing, to avoid cepstral time-aliasing.
- No IIR-for-LF split — the EARS correction is a gentle broadband trim, not a steep sub-20 Hz filter.

**Optional mode: complex FIR with the file's phase column** (REW-with-phase parity). Behind an advanced toggle; labeled **approximate** (REW warns its min-phase may be inaccurate above ~¼ fs and its cal-phase interpolation is undocumented). Adds latency and pre-ringing risk.

**Cal-file selection:** default **HPN** (mic-only); reading the type tag, warn (non-blocking) if **HEQ** is loaded as mic-cal (double-target risk, §3.7). Document the chain: *Bridge = mic-only correction; Dirac = headphone target.*

**Equivalence claim:** applying the cal as a real-time time-domain pre-filter is LTI and commutes with Dirac's later processing, so it's mathematically equivalent to REW post-applying the same curve — provided the FIR matches the curve in-band, doesn't truncate the band, and the ASRC is in-band transparent.

---

## 8. GUI

Single resizable window, modern dark theme. Top-to-bottom:

1. **Input row:** EARS device picker (stable-key based) + detected channel map (ch0→L, ch1→R) with a one-click L/R verify.
2. **Two cal slots:** "Left ear cal" and "Right ear cal" file pickers (drag-and-drop), each showing filename, parsed serial + type tag, and a **static FR thumbnail** of the loaded curve. HEQ-as-mic-cal shows a small warning badge.
3. **Combine mode** selector, ordered by rigor, with the recommended TwoPass workflow surfaced and a one-line method hint.
4. **Output row:** virtual-device picker (recommended sinks tagged) + the "set this device's capture side as Dirac's Recording device" hint + preflight warnings (Communications default / enhancements / endpoint busy).
5. **Sample-rate + bit-depth** selectors: the rate menu offers the **selected input's native rates** (48 kHz for the original EARS; 44.1–192 kHz for EARS Pro); a rate the input can't do natively shows a **resample warning**. A **bit-depth** control (16 / 24-bit) sets the virtual-sink output format.
6. **Meters:** large L/R input meters + mono output meter with clip LEDs; live xrun/slip indicator and a "clean capture" status light.
7. **Transport:** Start/Stop bridge + status (running, rate, latency, xruns).
8. **Advanced disclosure:** cal phase mode, FIR length, output trim, `DAUDIO_WASAPI_NON_EXCLUSIVE` guidance, macOS aggregate-device controls.

Settings persist between launches.

---

## 9. Sample-rate policy

The rate menu is **driven by the selected input device's actual capabilities**:
- **Original EARS** → native **48 kHz** only (default). Any other rate shows a **resample warning** (the 48 kHz capture would be resampled with no fidelity benefit) and is offered only for forced-rate scenarios. 44.1 kHz is exposed only if the unit's USB descriptor reports it.
- **EARS Pro** → **44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz** are all native (no resample); the FIR is designed at the chosen rate (§7) and the ClockBridge still resamples to the *output* device's clock.
- **Bit depth:** the chain is float32 internally. The user selects **16 / 24 / 32-bit** for the virtual-sink output (24-bit default; 32-bit only where the sink accepts it, e.g. BlackHole — no measurement benefit beyond 24-bit, whose ~144 dB range is already well below any mic self-noise); the input runs at its native depth (EARS 24-bit; EARS Pro 16/24/32-bit).
- A rate/bit-depth the **selected input** can't do natively is the only thing that triggers the resample/format warning — the menu reflects reality per device, not a fixed list.

---

## 10. Virtual routing specifics
- Write the calibrated mono **identically to both channels** of the stereo virtual device.
- Keep strictly to the virtual device's **render** side; never open its capture endpoint.
- VB-CABLE: stereo/48 kHz default; bit-exact only when both I/O match its (admin-set) Internal SR; supports WASAPI exclusive — but we open it **shared**. VoiceMeeter forces a single engine rate and is the least transparent → deprioritize as the auto-detect target.
- BlackHole (macOS): no internal SRC; all connected apps must match the Audio-MIDI-Setup nominal rate; set the rate before opening; a rate change requires full device close/reopen. Do **not** hog a virtual device on macOS.

---

## 11. Error handling
- Cal parse errors: reject with line context; keep the previous valid FIR loaded.
- Device unavailable / disconnect: stop cleanly, surface, attempt re-bind by stable key.
- Rate unsupported by a device / exclusive-open rejection: graceful fallback to shared; clear message.
- Output device == any EARS endpoint, or virtual capture busy: block with explanation.
- Mid-sweep xrun/drift beyond threshold: flag the measurement as suspect.

---

## 12. Testing strategy
- **FRD parser** unit tests (headers, comments, malformed lines, non-monotonic freq, the actual 000-0000 files).
- **FIR design** test: the designed FIR's measured magnitude matches the (inverse) cal curve within tolerance across the band; min-phase property; no pre-ringing.
- **Combine math** tests (TwoPass / Average / Sum levels and channel routing).
- **Offline golden test:** feed a known sweep WAV through the full graph (parse → FIR → combine → mono-duplicate) and compare output to a reference.
- **Realtime-safety check:** assert no allocations/locks/syscalls in the audio callbacks (audio-thread guard in debug).
- **Clock-bridge test:** simulate drifting clocks; assert the FIFO+ASRC holds fill without underrun/overrun and stays in-band transparent; assert no oscillation under a step drift.

---

## 13. Open validation items (bench gates before shipping claims)

These came out of the verification pass and must be resolved empirically; several are existential to the product concept:

1. **Named virtual cable end-to-end smoke test** — confirm VB-CABLE Output (Windows) and BlackHole 2ch (macOS) actually appear in DLCT's recording list and complete a full headphone measurement. (Strongly inferred, not directly documented.)
2. **Dirac stereo-channel handling for L ≠ R** — confirm whether DLCT reads ch0, sums (+6 dB), or averages a stereo capture; the dual-mono mitigation makes the *signal* safe, but absolute-SPL targets need a known-level-tone calibration to learn Dirac's summing behavior.
3. **Cal-file polarity** — REW round-trip on a known reference to confirm inverse (`−dB`) vs as-is for the miniDSP HPN/HEQ files.
4. **Exclusive-mode rate negotiation** — verify DLCT (WASAPI exclusive) accepts the chosen virtual cable at 44.1/48 kHz without rejecting/locking the rate; verify EARS grants 48 kHz reliably (else shared fallback).
5. **macOS track** — Aggregate Device creation (`AudioHardwareCreateAggregateDevice`, not wrapped by public JUCE API: lifecycle/permissions/teardown), drift behavior vs Dirac's missing-sample detector, BlackHole/Loopback enumeration in DLCT.
6. **Inter-clock drift magnitude** — measure real drift over a sweep to confirm whether a simple buffer suffices or full ASRC is mandatory (expected mandatory on Windows); tune the control loop to avoid oscillation.
7. **Dirac analysis-window length** at worst-case bridge+cable delay (theory says fine; window-length-dependent; re-verify per Dirac version).
8. **Version drift** — all Dirac findings are anchored to Dirac Live **3.11** (Nov 2024); re-check the changelog each release (exclusive-mode default and the missing-sample detector have both changed within 3.x).

### Key citations
- Dirac Live 3.11 changelog (WASAPI exclusive default + `DAUDIO_WASAPI_NON_EXCLUSIVE=ON` + missing-sample detection): https://helpdesk.dirac.com/en/dirac-live/Dirac-Live-311-LATEST-Software-Changelog-bfed
- Dirac recording-device enumeration / third-party mic acceptance: https://helpdesk.dirac.com/en/dirac-live/Where-should-I-connect-the-microphone-bfe7
- Official miniDSP "Using Dirac Live with EARS" single-side method: https://www.minidsp.com/applications/headphone-equalization/using-dirac-live-2-with-ears
- miniDSP EARS Product Brief (UAC1, 24-bit/48 kHz, DIP gain, per-channel cal): https://www.minidsp.com/images/documents/Product%20Brief-EARS.pdf
- REW cal-file convention (subtracted/corrective; phase optional): https://www.roomeqwizard.com/help/help_en-GB/html/calfiles.html
- Minimum-phase FIR via real cepstrum (CCRMA / J.O. Smith): https://ccrma.stanford.edu/~jos/sasp/Minimum_Phase_Filter_Design.html
- JUCE `hasSeparateInputsAndOutputs()` (ASIO false): https://docs.juce.com/master/classAudioIODeviceType.html
- JUCE CoreAudio combiner does no drift correction: https://forum.juce.com/t/losing-audio-sync-with-coreaudio/54792
- `juce::dsp::Convolution` realtime-safety / wait-free `loadImpulseResponse()`: https://docs.juce.com/master/classjuce_1_1dsp_1_1Convolution.html
- VB-CABLE Reference Manual (stereo/48 kHz, fixed Internal SR): https://vb-audio.com/Cable/VBCABLE_ReferenceManual.pdf
- BlackHole (no internal SRC; match AMS nominal rate): https://github.com/ExistentialAudio/BlackHole

---

## 14. Tech stack & platforms
- **Language/framework:** C++ with **JUCE ≥ 7** (do not use < 6, where `Convolution::process()` allocated on the audio thread).
- **Build:** CMake; targets Windows (WASAPI) and macOS (CoreAudio). No ASIO for the cross-device split.
- **Repo home:** `C:\Users\Shaya\OneDrive\Documents\EARS_program`.

---

## 15. Milestones (high level — detailed plan follows in writing-plans)
1. Project scaffold (JUCE + CMake), device enumeration, EARS @ 48 kHz capture + virtual render passthrough (no DSP), with FIFO+ASRC clock bridge.
2. FRD parser + min-phase FIR designer + per-channel convolution + combine modes.
3. GUI: device pickers, cal slots + thumbnails, combine selector, meters, rate menu + warnings, preflight checks.
4. Health monitoring (xrun/slip, L/R verify, clip), settings persistence.
5. macOS aggregate-device path; cross-platform polish.
6. Bench-validate the §13 open items; tests throughout.

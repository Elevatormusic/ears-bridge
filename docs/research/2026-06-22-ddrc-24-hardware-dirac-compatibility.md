# EARS Bridge + hardware Dirac (miniDSP DDRC-24): compatibility research

**Date:** 2026-06-22 · **Status:** research / not yet built · **Sources:** primary (DDRC-24 User Manual + Product Brief, read directly) + 3 web-research passes (cited below)

## Question

EARS Bridge has been built assuming **Dirac Live runs as software on the same PC** as the app. Can a
user calibrate PC headphones when **Dirac runs on a hardware miniDSP processor** (a DDRC-24) instead?
If not, why — and how would we accommodate them?

## TL;DR verdict

**Yes — the calibration works, but our measurement-*grading* layer does not.** The mic-side bridge is
fully compatible (and the DDRC-24 uses the *modern* Dirac Live, not legacy DLCT). The catch, confirmed
from the DDRC-24 manual: **the Dirac test sweep is generated *inside* the DDRC-24, not played from a PC
output device**, so our WASAPI-loopback reference monitor has no PC-side digital sweep to capture. A
"hardware Dirac" mode would keep the core measurement and **gracefully degrade the grade**.

## What the primary source confirms

From the **DDRC-24 User Manual** (Dirac Live chapter) and **Product Brief**:

1. **It's the modern Dirac Live v3 app** — *"the Dirac Live calibration program from Dirac Research"*,
   installed as **`diraclive-latest.exe`** (Win) / **`DiracLive v3.x.y Setup Darwin`** (mac), launched via
   Device Console's **Start Calibration** button. This is the **same app EARS Bridge already targets**
   (corrects the web guess that the DDRC-24 was stuck on legacy DLCT v1).
2. **The measurement mic is a PC-side, selectable recording device** — *"Check that UMIK-1 is set as the
   **default recording device**… click on it and then on **Set Default**."* UMIK plugs into the PC USB,
   not the box. → **EARS Bridge's virtual cable can stand in as Dirac's mic exactly as it does today.**
   (Two USB ports needed: DDRC-24 + UMIK/EARS.)
3. **Stereo (2-channel) Dirac** — *"there are two channels of Dirac Live correction… then routed to the
   four output channels."* L/R = per-ear for headphones. The DDRC-24 has **2 analog in / 4 analog out**
   (RCA), 48 kHz internal, SHARC ADSP-21489, XMOS async USB Audio Class 2 / ASIO, a **routing matrix**,
   4 presets. *"A computer is only required for the initial configuration and for USB audio streaming."*
4. **Direct USB only** — *"Dirac Live requires a direct USB connection… cannot be used via the Wi-DG."*
5. **No headphone content in the DDRC-24 manual at all** (all "listening area" / 9–17 speaker sweeps) —
   headphone use stays the **off-label EARS app-note** route (route both channels to one earcup via the
   matrix mixer, single-ear, "well-matched headphones"). EARS Bridge's **Auto per-ear is strictly better**
   than that hack.

## The blocker the web sources missed: the sweep is generated inside the box

Manual, verbatim:

> *"Usually, you do not need to change any of the audio connections, as the **Dirac Live test signals are
> generated inside the miniDSP processor**."*
> *"the Dirac Live test signal **originates in the Dirac Live blocks** and therefore passes through the
> routing matrix and the output channel processing."*

For **software** Dirac, the sweep is a PC render stream — which is exactly what EARS Bridge's
**reference monitor** loopback-captures to grade each measurement. On the DDRC-24 the sweep **never exists
as a PC audio stream**; it's synthesised in the SHARC and leaves via the analog RCA outputs. Therefore
there is **nothing to WASAPI-loopback** for the digital reference.

## Feature-by-feature compatibility

| EARS Bridge feature | DDRC-24 (hardware Dirac) |
|---|---|
| **Core bridge** (EARS mic → per-ear FIR → mono → virtual cable → Dirac records) | ✅ Works — sweep reaches the headphones via OUT1/OUT2 → headphone amp; EARS captures it acoustically; Dirac records the cable (set as the default recording device) |
| **Auto per-ear** (2-ch Dirac = L/R) | ⚠️ Likely works (Dirac hard-pans L/R to the two output channels), **but** verify our active-ear detection isn't tied to the loopback — see open items |
| **Reference monitor / IR-quality grade** (loopback of Dirac's digital sweep) | ❌ **Breaks** — no PC-side digital sweep to capture as the reference |
| **3-clock phase-drift worry** (from the prior synthesis) | Re-shaped: the DDRC-24 *is* the sweep clock (internal) + EARS is the capture clock — a 2-clock deconvolution Dirac itself handles |
| **WASAPI-exclusive recording** (the 600007 / VB-CABLE shared-mode issue) | Same as today — Dirac still records the mic/cable in WASAPI on the PC |

## How we'd accommodate them (plan, not yet built)

1. **Detect the hardware-Dirac case** (Dirac's output is a hardware unit with no loopback-able PC sweep)
   and **gracefully degrade**: keep the core bridge, **disable the reference monitor**, and tell the user
   *"measurement grading isn't available when Dirac runs on a hardware processor"* — instead of silently
   showing a never-green grade.
2. **Give grading a non-loopback path** if we want it back: grade against a **stored/canonical ESS
   reference**, or fold in the **rig-characterization sub-project (D)** — since we can't observe the box's
   internal sweep, characterise against a *known* sweep instead.
3. **Verify Auto per-ear's active-ear detection** is not loopback-dependent; fall back to
   mic-level / earcup-energy detection if it is.
4. **Docs / setup mode:** headphones on the DDRC-24 analog out → headphone amp; the EARS Bridge virtual
   cable set as the **default recording device**; modern Dirac Live v3; **skip miniDSP's matrix single-ear
   hack** in favour of our per-ear.

## Open items / on-device validation owed

- **Auto per-ear active-ear detection** — confirm in code whether it depends on the loopback reference
  (if so, it needs a non-loopback fallback for the hardware path). [[ears-dirac-sweep-model]]
- **The graceful-degrade detection** — how does EARS Bridge tell "hardware Dirac" from "software Dirac"?
  (No loopback-able sweep on the selected output is one signal; the output being a recognised miniDSP
  USB device is another.)
- **Stored/canonical-reference grading** feasibility for the hardware path (ties to sub-project D).
- All of the above need a real DDRC-24 to confirm.

## Sources

**Primary (read directly):**
- miniDSP DDRC-24 User Manual (Dirac Live chapter: app = Dirac Live v3 / `diraclive-latest.exe`; UMIK as
  default recording device; "test signals are generated inside the miniDSP processor"; stereo Dirac; USB-only).
- miniDSP DDRC-24 Product Brief (2-in/4-out, 48 kHz, SHARC ADSP-21489, XMOS async USB Class 2/ASIO,
  matrix mixer, 4 presets, "computer only required for initial configuration + USB streaming").

**Secondary (web research, cross-corroborated):**
- [DDRC-24 product page](https://www.minidsp.com/products/minidsp-in-a-box/ddrc-24) ·
  [Dirac Live User Manual PDF](https://www.awe-europe.com/files//af38822b-a853-4081-87dd-3067092d0829/DiracLiveUserManual-1.pdf)
  (recording-device picker lists virtual devices — BlackHole / Aggregate)
- [Using Dirac Live to tune headphones with EARS](https://www.minidsp.com/applications/headphone-equalization/using-dirac-live-2-with-ears) ·
  [EARS with DDRC-22D thread](https://www.minidsp.com/community/threads/ears-with-ddrc-22d.16423/) (the matrix single-ear hack)
- [Dirac: DLCT vs Dirac Live](https://helpdesk.dirac.com/en/dirac-room-correction/What-are-the-differences-between-Dirac-Live-and-DLCT-1x-87af) ·
  [Understanding Dirac versions on miniDSP](https://support.minidsp.com/support/solutions/articles/47001188187-understanding-the-various-dirac-live-versions-on-minidsp-platforms)
- Dirac headphone *products* are a separate B2B/embedded line (Opteo/Virtuo), unrelated to running Dirac
  Live on a miniDSP box: [Dirac headphone B2B](https://www.dirac.com/dirac-for-business/headphone-audio/)

See also: [[ears-bridge-project]], [[ears-dirac-sweep-model]], [[ears-manual-spl-and-caltypes]],
[[ears-dirac-phase-precision]].

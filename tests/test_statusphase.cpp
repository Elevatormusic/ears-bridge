#include <catch2/catch_test_macros.hpp>
#include "gui/LiveInputStatus.h"

using eb::StatusPhase;
using eb::statusPhase;
using eb::sweepActiveRelease;

// ---- statusPhase: the pure decision behind updateStatusLine's persistent-vs-waiting ladder -------------
// Args: statusPhase(running, hasGrade, sweepActive, anyHardError).

TEST_CASE("statusPhase: a captured grade (not sweeping, no error) is Captured, NOT Waiting (Bug A)") {
    // The level arm never arms on a gradual Dirac log-sweep, so the session stays Idle/Preflight and the old
    // ladder showed "waiting" forever. With a grade present and the sweep not sounding we must show Captured.
    CHECK (statusPhase (/*running*/ true, /*hasGrade*/ true, /*sweepActive*/ false, /*anyHardError*/ false)
           == StatusPhase::Captured);
}

TEST_CASE("statusPhase: a captured grade with a hard error (e.g. clipped/low-SNR) -> the error WINS") {
    // A clipped or low-SNR captured sweep must show the warning, never "safe to run the next sweep".
    CHECK (statusPhase (true, /*hasGrade*/ true, /*sweepActive*/ false, /*anyHardError*/ true)
           == StatusPhase::Error);
}

TEST_CASE("statusPhase: running, a reference loaded, nothing graded yet, not sweeping -> Waiting") {
    // The genuine pre-first-sweep wait: no grade this run and the sweep isn't sounding.
    CHECK (statusPhase (true, /*hasGrade*/ false, /*sweepActive*/ false, /*anyHardError*/ false)
           == StatusPhase::Waiting);
}

TEST_CASE("statusPhase: an active sweep -> LiveSweep, even with a stale grade present") {
    // While Dirac is sounding the live readout takes over so the user sees the level / catches a clip.
    CHECK (statusPhase (true, /*hasGrade*/ false, /*sweepActive*/ true,  /*anyHardError*/ false)
           == StatusPhase::LiveSweep);
    CHECK (statusPhase (true, /*hasGrade*/ true,  /*sweepActive*/ true,  /*anyHardError*/ false)
           == StatusPhase::LiveSweep);
}

TEST_CASE("statusPhase: a hard error beats LiveSweep") {
    CHECK (statusPhase (true, true, /*sweepActive*/ true, /*anyHardError*/ true) == StatusPhase::Error);
}

// ---- sweepActiveRelease: the attack/release HOLD that keeps the live readout engaged across the gap (Bug B-2)

TEST_CASE("sweepActiveRelease: above the gate re-arms the hold to full") {
    CHECK (sweepActiveRelease (/*release*/ 0,  /*aboveGate*/ true, /*holdTicks*/ 45) == 45);
    CHECK (sweepActiveRelease (/*release*/ 12, /*aboveGate*/ true, /*holdTicks*/ 45) == 45);
}

TEST_CASE("sweepActiveRelease: a sub-gate tick BLEEDS the hold down by one, never snapping to 0") {
    // This is the Bug B-2 core: the quiet L<->R inter-sweep gap (and brief dips) decrement instead of resetting.
    CHECK (sweepActiveRelease (/*release*/ 45, /*aboveGate*/ false, 45) == 44);
    CHECK (sweepActiveRelease (/*release*/ 1,  /*aboveGate*/ false, 45) == 0);
    CHECK (sweepActiveRelease (/*release*/ 0,  /*aboveGate*/ false, 45) == 0);   // already released stays at 0
}

TEST_CASE("sweepActiveRelease: a single sub-gate tick during a held sweep stays ENGAGED (release hold)") {
    // Simulate: the sweep was sounding (hold full at 45), then ONE quiet tick (the inter-sweep gap). The hold
    // is still > 0, so the GUI keeps owning the live readout -> the status does NOT flip out and back.
    const int afterOneQuietTick = sweepActiveRelease (/*release*/ 45, /*aboveGate*/ false, 45);
    CHECK (afterOneQuietTick > 0);                       // still engaged
    // And it only fully releases after ~holdTicks consecutive quiet ticks (here we step it down to 0).
    int rel = 45;
    for (int i = 0; i < 45; ++i) rel = sweepActiveRelease (rel, /*aboveGate*/ false, 45);
    CHECK (rel == 0);                                    // released after the full hold elapses
    // A single above-gate tick mid-gap re-arms the whole hold again (the sweep resumed).
    CHECK (sweepActiveRelease (/*release*/ 3, /*aboveGate*/ true, 45) == 45);
}

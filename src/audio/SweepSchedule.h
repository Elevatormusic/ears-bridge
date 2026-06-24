#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "audio/EngineTypes.h"     // eb::Ear (canonical home; shared with LrVerify)
#include "audio/NoiseFloorMath.h"  // eb::rmsLinearOver / linToDb (shared block-RMS + dB math)

// AutoPerEar hardening (P0-06) — the LEARNED sweep schedule.
//
// Dirac's per-position headphone measurement is a hard-panned burst on its render loopback. On-device
// (eb_diag pancheck, 2026-06-23) the structure was confirmed: [L sweep ~5s][gap ~0.5s][R sweep ~5s]
// [gap ~0.5s][L REFERENCE sweep ~5s] — order L,R,L (the trailing first-channel drift reference), with
// ~104.5 dB channel isolation (active ~-15 dBFS, silent at the -120 floor). A brief stereo level tone
// (~1s, |L-R| < 6 dB, ~-40 dBFS) precedes the sweeps and must be IGNORED. Segment durations are config-
// dependent (~5s here vs ~10s elsewhere), so the schedule LEARNS the relative structure, never hardcodes
// seconds.
//
// extractSchedule reads the FULL stereo loopback (BEFORE per-channel trimming) and recovers that schedule.
// Pure/offline — the clean 104.5 dB-isolated loopback makes simple block-RMS + an absolute level/pan test
// decisive (no FFT, no noise-floor estimate needed). The ROUTER (ScheduledEarRouter, separate unit) then
// drives the per-ear routing from this schedule's TIMING, so the quiet sweep extremes are routed by the
// clock, not by the faint mic envelope, and the trailing L is handled.
namespace eb {

struct SweepSegment {
    Ear    ear;            // which earcup Dirac drives during this segment
    double durationSec;    // measured run length
};

struct SweepSchedule {
    std::vector<SweepSegment> segments;       // in time order, e.g. [L, R, L]
    std::vector<double>       gapsSec;        // gap AFTER segment[i]; size == max(0, segments.size()-1).
                                              // The router uses this (its dwell adapts to the learned gap length).
    bool   valid          = false;            // true iff >= 2 segments were recovered (else: mic-envelope fallback)
};

// Learn the schedule from the stereo loopback `L`/`R` (n samples at `rate`). Block-RMS over blockSec windows:
//   - a block is ACTIVE when its louder channel dB exceeds activeDb;
//   - a block is a SWEEP block when it is active AND hard-panned (|Ldb - Rdb| >= panMinDb); its ear is the
//     louder channel. The pre-sweep stereo tone is active-but-not-panned -> never a sweep block -> ignored.
// Consecutive same-ear sweep blocks coalesce into a run; a run becomes a SweepSegment iff it is >= minSegmentSec
// (rejects blips). gapsSec[i] = the time between the end of segment i and the start of segment i+1.
// Note: an ABSOLUTE activeDb (not a floor+margin) is used deliberately — the loopback's ~104 dB isolation puts
// the sweep (~-18 dB RMS) and the silence (~-120 dB) on opposite sides of any threshold in [-90, -40], so the
// gate is not sensitive. valid iff >= 2 segments. Bad args / no segments -> { valid=false }.
[[nodiscard]] inline SweepSchedule extractSchedule (const float* L, const float* R, int n, double rate,
                                                    double blockSec     = 0.05,
                                                    double panMinDb     = 90.0,
                                                    double minSegmentSec = 2.0,
                                                    double activeDb     = -60.0) {
    SweepSchedule out;
    if (L == nullptr || R == nullptr || n <= 0 || rate <= 0.0 || blockSec <= 0.0) return out;
    const int blk = std::max (1, (int) std::lround (blockSec * rate));
    const int nb  = n / blk;   // whole blocks only

    // dB of the block RMS, via the shared NoiseFloorMath helpers (NaN/Inf-skipping so one bad sample can't
    // fracture a segment - clean PCM loopback never has them, but be robust).
    auto rmsDb = [] (const float* x, int i0, int len) -> double {
        return linToDb (rmsLinearOver (x, i0, i0 + len));
    };

    struct Run { int ear; int startBlk; int endBlk; };   // [startBlk, endBlk)
    std::vector<Run> runs;
    int curEar = -1, curStart = -1;
    auto closeRun = [&] (int endBlk) {
        if (curEar >= 0 && curStart >= 0) runs.push_back ({ curEar, curStart, endBlk });
        curEar = -1; curStart = -1;
    };
    for (int b = 0; b < nb; ++b) {
        const int    i0     = b * blk;
        const double ldb    = rmsDb (L, i0, blk);
        const double rdb    = rmsDb (R, i0, blk);
        const double louder = std::max (ldb, rdb);
        int cls;                                                  // -1 quiet, 0 sweep-L, 1 sweep-R, 2 active-unpanned
        if      (louder <= activeDb)                  cls = -1;
        else if (std::abs (ldb - rdb) >= panMinDb)    cls = (ldb > rdb) ? 0 : 1;
        else                                          cls = 2;
        if (cls == 0 || cls == 1) {
            if (cls != curEar) { closeRun (b); curEar = cls; curStart = b; }   // ear change / fresh run
        } else {
            closeRun (b);                                                       // quiet or unpanned ends the run
        }
    }
    closeRun (nb);

    std::vector<int> segStartBlk, segEndBlk;
    for (const auto& r : runs) {
        const double dur = (double) (r.endBlk - r.startBlk) * (double) blk / rate;
        if (dur + 1.0e-9 >= minSegmentSec) {
            out.segments.push_back ({ (Ear) r.ear, dur });
            segStartBlk.push_back (r.startBlk);
            segEndBlk.push_back (r.endBlk);
        }
    }
    for (std::size_t i = 1; i < out.segments.size(); ++i)
        out.gapsSec.push_back ((double) (segStartBlk[i] - segEndBlk[i - 1]) * (double) blk / rate);
    out.valid = out.segments.size() >= 2;
    return out;
}

} // namespace eb

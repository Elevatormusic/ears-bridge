#pragma once
#include "audio/Hysteresis.h"     // BandHysteresis (generic anti-flicker)
#include "audio/RefMonitor.h"     // QualityBand, the metric edge arrays + margins, snr/thdBandFromIndex

namespace eb {

// Per-EAR anti-flicker smoother for the 3 measurement-quality bands (sweepSNR / IR-SNR / THD) across
// consecutive grades. Holds one BandHysteresis per metric, keyed on the metric's ascending edge array +
// hysteresis margin (from RefMonitor.h). The per-metric color dots render these SMOOTHED bands so a value
// sitting on a band edge does not flicker green/orange between measurements. The raw dB/% is shown beside
// each dot regardless (the honest signal). reset() on Start/Stop re-arms the smoothing for a fresh run.
//
// NOTE the index->band mapping differs by metric direction: SNR metrics are "higher is better"
// (snrBandFromIndex: 0=Red,1=Orange,2=Green); THD is "lower is better" (thdBandFromIndex: 0=Green,1=Orange,2=Red).
struct GradeBandSmoother {
    BandHysteresis snr { kSweepSnrEdges, 2, kSnrHystDb };   // {18,25} dB, 1 dB margin
    BandHysteresis ir  { kIrSnrEdges,    2, kSnrHystDb };   // {35,50} dB, 1 dB margin
    BandHysteresis thd { kThdEdges,      2, kThdHystPct };  // {3,10} %, 0.5% margin

    struct Bands { QualityBand sweepSnr = QualityBand::Unknown;
                   QualityBand irSnr    = QualityBand::Unknown;
                   QualityBand thd      = QualityBand::Unknown; };

    // Feed one fresh grade's raw metrics; returns the SMOOTHED bands. An invalid sweepSNR returns Unknown for
    // that metric and does NOT advance its hysteresis (so the next valid reading resumes from the last band).
    Bands update (float sweepSnrDb, bool sweepSnrValid, float irSnrDb, float thdPct) {
        Bands b;
        b.sweepSnr = sweepSnrValid ? snrBandFromIndex (snr.update (sweepSnrDb)) : QualityBand::Unknown;
        b.irSnr    = snrBandFromIndex (ir.update (irSnrDb));
        b.thd      = thdBandFromIndex (thd.update (thdPct));
        return b;
    }

    void reset() noexcept { snr.reset(); ir.reset(); thd.reset(); }
};

} // namespace eb

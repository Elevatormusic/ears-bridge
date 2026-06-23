#pragma once

namespace eb {

// Generic, reusable anti-flicker hysteresis for a value classified into ORDERED bands by N ascending
// `edges`. Pure + header-only; usable for ANY banded metric (SNR dB, THD %, levels, temperatures, …).
// The band SEMANTICS (which index means "good") live with the CALLER — this only decides the band index.
//
// Bands (>= lower-inclusive):
//   index 0   = value <  edges[0]
//   index i   = edges[i-1] <= value < edges[i]
//   index N   = value >= edges[N-1]
// (A "good is lower" metric like THD just maps the index the other way. Real measured values never land
//  exactly on an edge, so the inclusive side is immaterial in practice.)
//
// HYSTERESIS: with a known `prevBand`, the value must move PAST the edge separating prevBand from its
// neighbour by `margin` (in value units) before the band flips — a symmetric dead-band of width `margin`
// on each side of every edge. `prevBand < 0` means "no history": return the raw hard band.
[[nodiscard]] inline int hysteresisBand (float value, const float* edges, int nEdges,
                                         int prevBand, float margin) noexcept {
    if (edges == nullptr || nEdges <= 0) return 0;

    int raw = 0;
    for (int i = 0; i < nEdges; ++i)
        if (value >= edges[i]) raw = i + 1;

    if (prevBand < 0) return raw;                     // no history -> the raw hard band
    if (prevBand > nEdges) prevBand = nEdges;         // guard: an out-of-range prevBand must not index past edges[]
    if (raw == prevBand) return raw;

    if (raw > prevBand)   // moving UP: clear the edge ABOVE prevBand (edges[prevBand]) by +margin, else hold
        return (value >= edges[prevBand] + margin) ? raw : prevBand;

    // moving DOWN: clear the edge BELOW prevBand (edges[prevBand-1]) by -margin, else hold
    return (value < edges[prevBand - 1] - margin) ? raw : prevBand;
}

// Stateful convenience wrapper: remembers the last band across calls. The typical reuse site is one
// instance per metric per channel (e.g. per-ear sweepSNR/IR-SNR/THD smoothing across consecutive grades).
class BandHysteresis {
public:
    // `edges` must outlive this object (callers pass a static constexpr array). `margin` in value units.
    BandHysteresis (const float* edges, int nEdges, float margin) noexcept
        : edges_ (edges), nEdges_ (nEdges), margin_ (margin) {}

    int update (float value) noexcept {
        last_ = hysteresisBand (value, edges_, nEdges_, last_, margin_);
        return last_;
    }
    void reset() noexcept { last_ = -1; }                 // forget history; next update() takes the raw band
    [[nodiscard]] int band() const noexcept { return last_; }   // -1 until the first update()

private:
    const float* edges_;
    int   nEdges_;
    float margin_;
    int   last_ = -1;
};

} // namespace eb

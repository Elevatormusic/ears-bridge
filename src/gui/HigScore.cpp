#include "gui/HigScore.h"
#include "gui/HigThresholds.h"
#include <cmath>

namespace eb::hig {
namespace {

struct El {
    juce::String id, type, role, label, value, fg, bg;
    double x{}, y{}, w{}, h{}, fontPt{};
    bool bold{}, visible{}, showing{}, measurable{}, fgOk{}, bgOk{}, textOverflows{};
};

El parse (const juce::var& v) {
    El e;
    const auto b = v.getProperty ("bounds", {});
    e.id = v.getProperty ("id", {}).toString();          e.type  = v.getProperty ("type", {}).toString();
    e.role = v.getProperty ("role", {}).toString();      e.label = v.getProperty ("label", {}).toString();
    e.value = v.getProperty ("value", {}).toString();    e.fg    = v.getProperty ("fg", {}).toString();
    e.bg = v.getProperty ("bg", {}).toString();
    e.x = (double) b.getProperty ("x", 0); e.y = (double) b.getProperty ("y", 0);
    e.w = (double) b.getProperty ("w", 0); e.h = (double) b.getProperty ("h", 0);
    e.fontPt = (double) v.getProperty ("fontPt", 0);     e.bold    = v.getProperty ("bold", false);
    e.visible = v.getProperty ("visible", false);        e.showing = v.getProperty ("showing", false);
    e.measurable = v.getProperty ("measurable", false);
    e.fgOk = v.getProperty ("fgIntrospectable", false);  e.bgOk = v.getProperty ("bgIntrospectable", false);
    e.textOverflows = v.getProperty ("textOverflows", false);
    return e;
}

// --- WCAG 2.x contrast: byte-faithful port of apple-hig wcag-contrast.mjs (channel/relativeLuminance/ratio).
// Operates on the 6-hex RRGGBB strings the probe's resolveColours already produced; no alpha (the native path
// emits opaque registered colours), so blendOver is intentionally NOT ported.
double chan (double c8) {
    const double c = c8 / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4);
}
double lumOf (const juce::String& hex) {
    auto h = hex.trimCharactersAtStart ("#");
    if (h.length() == 3)
        h = juce::String::charToString (h[0]) + h[0] + juce::String::charToString (h[1]) + h[1]
          + juce::String::charToString (h[2]) + h[2];
    auto comp = [&] (int i) { return (double) h.substring (i, i + 2).getHexValue32(); };
    return 0.2126 * chan (comp (0)) + 0.7152 * chan (comp (2)) + 0.0722 * chan (comp (4));
}
bool isLarge (const El& e) { return e.fontPt >= kLargeFontPt || (e.bold && e.fontPt >= kLargeBoldFontPt); }

struct Box { double left, top, right, bottom; };

// interactive(): native-review.mjs ORs a type regex with a role regex - /button|toggle|slider|combo/i on
// type, /button|link|slider|checkbox/i on role. ToggleButton matches via "toggle"/"button"; ComboBox via
// "combo"; Label matches neither.
bool interactive (const El& e) {
    // case-insensitive substring, mirroring native-review's /.../i regexes (so a future probe role like
    // "BUTTON" / "my-button" / a real AccessibilityHandler role agrees with the JS - not just the current
    // lowercase-token probe output).
    const auto t = e.type.toLowerCase();
    for (auto* tok : { "button", "toggle", "slider", "combo" })
        if (t.contains (tok)) return true;
    const auto r = e.role.toLowerCase();
    for (auto* tok : { "button", "link", "slider", "checkbox" })
        if (r.contains (tok)) return true;
    return false;
}

} // namespace

// P4 T4: moved OUT of the anonymous namespace above (it sat there with internal linkage) to
// eb::hig scope, matching the HigScore.h declaration — the token tests and the state-sweep
// checker consume the same definition scoreDescriptor uses. lumOf/chan stay file-local.
double contrastRatio (const juce::String& a, const juce::String& b) {
    const double l1 = lumOf (a), l2 = lumOf (b);
    const double hi = juce::jmax (l1, l2), lo = juce::jmin (l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

std::vector<Finding> scoreDescriptor (const juce::var& root) {
    std::vector<Finding> out;
    auto* arr = root.getProperty ("elements", {}).getArray();
    if (arr == nullptr) return out;

    std::vector<El> els;
    els.reserve ((size_t) arr->size());
    for (auto& v : *arr) els.push_back (parse (v));

    // --- contrast (native-review.mjs contrastFindings) ---
    for (auto& e : els) {
        if (! e.measurable || ! e.fgOk || ! e.bgOk) continue;        // never score a custom-painted node
        if (e.label.isEmpty() && e.value.isEmpty()) continue;
        const double ratio = contrastRatio (e.fg, e.bg);
        const double floor = isLarge (e) ? kContrastFloorLarge : kContrastFloorNormal;
        if (ratio < floor)
            out.push_back ({ "contrast", ratio < floor - kHighSeverityDelta ? "high" : "medium", e.id,
                "text contrast " + juce::String (ratio, 2) + ":1 is below " + juce::String (floor, 1) + ":1" });
    }

    // --- geometry (native-review.mjs geometryFindings: overlap / duplicate / target-size / clip) ---
    auto rectOf = [] (const El& e) { return Box { e.x, e.y, e.x + e.w, e.y + e.h }; };
    auto overlaps = [] (const Box& a, const Box& b) {
        return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top; };
    auto depth = [] (const Box& a, const Box& b) {
        const double w = juce::jmax (0.0, juce::jmin (a.right, b.right) - juce::jmax (a.left, b.left));
        const double h = juce::jmax (0.0, juce::jmin (a.bottom, b.bottom) - juce::jmax (a.top, b.top));
        return juce::jmin (w, h); };
    auto contains = [] (const Box& p, const Box& q) {
        return p.left <= q.left && p.top <= q.top && p.right >= q.right && p.bottom >= q.bottom; };

    std::vector<const El*> vis;
    for (auto& e : els) if (e.visible && e.showing) vis.push_back (&e);

    for (size_t i = 0; i < vis.size(); ++i)
        for (size_t j = i + 1; j < vis.size(); ++j) {
            const El& a = *vis[i]; const El& b = *vis[j];
            const Box ra = rectOf (a), rb = rectOf (b);
            const bool ov = overlaps (ra, rb) && depth (ra, rb) > (double) kOverlapNoisePx;
            const bool nested = contains (ra, rb) || contains (rb, ra);
            const bool identical = a.type == b.type && a.label.isNotEmpty()
                                   && a.label == b.label && a.w == b.w && a.h == b.h;   // raw compare = JS ===
            if (identical)   // the identical branch SUPPRESSES the overlap finding for the same pair
                out.push_back ({ "duplicate", ov ? "high" : "medium", b.id,
                    "identical \"" + a.label + "\" (" + a.type + ") appears twice" });
            else if (ov && ! nested)
                out.push_back ({ "overlap", "medium", b.id,
                    b.id + " overlaps " + a.id + " by " + juce::String ((int) depth (ra, rb)) + "px" });
        }

    for (auto* e : vis)
        if (interactive (*e) && (e->w < (double) kMinTargetPx || e->h < (double) kMinTargetPx))   // raw < = JS
            out.push_back ({ "target-size", "medium", e->id,
                juce::String ((int) e->w) + "x" + juce::String ((int) e->h) + "px target below "
                + juce::String (kMinTargetPx) + "px" });

    for (auto* e : vis)
        if (e->textOverflows) {
            const auto txt = e->label.isNotEmpty() ? e->label : e->value;   // label||value, matching native-review
            out.push_back ({ "clip", "medium", e->id, "\"" + txt.substring (0, 32) + "\" overflows its bounds" });
        }

    return out;
}

std::vector<Finding> scoreMinFont (const juce::var& root) {
    std::vector<Finding> out;
    auto* arr = root.getProperty ("elements", {}).getArray();
    if (arr == nullptr) return out;
    for (auto& v : *arr) {
        const El e = parse (v);
        if (! (e.visible && e.showing)) continue;
        if (e.label.isEmpty() && e.value.isEmpty()) continue;
        if (e.fontPt > 0.0 && e.fontPt < kMinFontPt)
            out.push_back ({ "min-font", "medium", e.id,
                "text at " + juce::String (e.fontPt, 2) + "pt (probe points) is below the "
                + juce::String (kMinFontPt, 1) + "pt floor (macOS ramp legibility floor)" });
    }
    return out;
}

// ===== P4: the state-sweep checker (native-review.mjs stateFindings, tiers 1-2; v1.10.0) =========
// Transcribed from the plugin source with the semantics verified line-by-line. Two fidelity notes vs
// the plan's draft: (1) isPrimaryAction is the FULL upstream predicate (primary || prominent ||
// variant~=/prominent/i), not primary alone; (2) upstream runs tier 2 even when tier 1 found the
// control inert (no early continue) - outcome-equivalent given the tolerances (equal-within-2/0.01
// samples can never differ by the 0.75/0.05 louder margins), but mirrored exactly for parity.
// bg is accepted in the descriptor's schema form (hex string); the JS also tolerates an [r,g,b]
// array used by its internal test callers - our probe and fixtures only ever emit hex.
namespace {
constexpr const char* kSweepStates[] = { "normal", "over", "down", "disabled", "toggledOn", "toggledOff" };

struct Sample { bool ok = false; double r = 0, g = 0, b = 0, alpha = 1; juce::Array<juce::var> grid; };

Sample sampleOf (const juce::var& s) {
    Sample out;
    if (! s.isObject()) return out;
    const auto rgb = s.getProperty ("rgb", {});
    auto* a = rgb.getArray();
    if (a == nullptr || a->size() != 3) return out;
    out.r = (double) (*a)[0]; out.g = (double) (*a)[1]; out.b = (double) (*a)[2];
    out.alpha = s.hasProperty ("alpha") ? (double) s.getProperty ("alpha", 1.0) : 1.0;
    if (auto* g = s.getProperty ("grid", {}).getArray()) out.grid = *g;
    out.ok = true;
    return out;
}
bool measurable (const Sample& s) { return s.ok && s.alpha >= kSweepLowAlpha; }
double chDiff (const Sample& a, const Sample& b) {
    return juce::jmax (std::abs (a.r - b.r), std::abs (a.g - b.g), std::abs (a.b - b.b));
}
// samplesEqual: mean rgb within tol AND alpha within tol AND every co-indexed grid cell within tol
// (a missing grid on either side degrades to mean-only - never a forced difference).
bool samplesEqual (const Sample& a, const Sample& b) {
    if (! a.ok || ! b.ok) return false;
    if (chDiff (a, b) > (double) kSweepRgbTol) return false;
    if (std::abs (a.alpha - b.alpha) > kSweepAlphaTol) return false;
    const int n = juce::jmin (a.grid.size(), b.grid.size());
    for (int i = 0; i < n; ++i) {
        auto* ga = a.grid[i].getArray(); auto* gb = b.grid[i].getArray();
        if (ga != nullptr && gb != nullptr && ga->size() == 3 && gb->size() == 3) {
            const double d = juce::jmax (std::abs ((double) (*ga)[0] - (double) (*gb)[0]),
                                         juce::jmax (std::abs ((double) (*ga)[1] - (double) (*gb)[1]),
                                                     std::abs ((double) (*ga)[2] - (double) (*gb)[2])));
            if (d > (double) kSweepRgbTol) return false;
        }
    }
    return true;
}
// Hue in degrees, or -1 for achromatic (no meaningful hue). hueDelta = smallest angular difference;
// 0 when either side is achromatic (a gray has no hue to rotate).
double hueDeg (const Sample& s) {
    const double r = s.r / 255.0, g = s.g / 255.0, b = s.b / 255.0;
    const double mx = juce::jmax (r, juce::jmax (g, b)), mn = juce::jmin (r, juce::jmin (g, b)), c = mx - mn;
    if (c < 1.0e-6) return -1.0;
    double h;
    if (mx == r)      h = std::fmod ((g - b) / c, 6.0);
    else if (mx == g) h = (b - r) / c + 2.0;
    else              h = (r - g) / c + 4.0;
    return std::fmod (h * 60.0 + 360.0, 360.0);
}
double hueDelta (const Sample& a, const Sample& b) {
    const double ha = hueDeg (a), hb = hueDeg (b);
    if (ha < 0.0 || hb < 0.0) return 0.0;
    const double d = std::abs (ha - hb);
    return juce::jmin (d, 360.0 - d);
}
juce::String rgbHex (const Sample& s) {
    return "#" + juce::String::toHexString ((int) (s.r + 0.5)).paddedLeft ('0', 2)
               + juce::String::toHexString ((int) (s.g + 0.5)).paddedLeft ('0', 2)
               + juce::String::toHexString ((int) (s.b + 0.5)).paddedLeft ('0', 2);
}
// The FULL upstream predicate: descriptor `primary`/`prominent` flags or a variant naming "prominent".
bool isPrimaryAction (const juce::var& ev) {
    return (bool) ev.getProperty ("primary", false)
        || (bool) ev.getProperty ("prominent", false)
        || ev.getProperty ("variant", {}).toString().containsIgnoreCase ("prominent");
}
} // namespace

std::vector<Finding> scoreStateSweep (const juce::var& root) {
    std::vector<Finding> out;
    auto* els = root.getProperty ("elements", {}).getArray();
    if (els == nullptr) return out;
    for (auto& ev : *els) {
        const auto id = ev.getProperty ("id", {}).toString();
        const auto states = ev.getProperty ("states", {});
        if (! states.isObject()) continue;

        // ---- tier 1: inertness across ALL measurable swept states ----
        juce::StringArray present;
        Sample first; bool allEqual = true;
        for (auto* k : kSweepStates) {
            const auto s = sampleOf (states.getProperty (juce::Identifier (k), {}));
            if (! measurable (s)) continue;
            if (present.isEmpty()) first = s;
            else if (! samplesEqual (s, first)) allEqual = false;
            present.add (k);
        }
        if (present.size() >= 2 && allEqual) {
            if (present.size() == 2)
                out.push_back ({ "two-state-inert", "info", id,
                    "control renders identically across its 2 swept states (" + present.joinIntoString ("/")
                    + ") - may be sanctioned; verify intent" });
            else
                out.push_back ({ "unstyled-control-states",
                    isPrimaryAction (ev) ? "high" : "medium", id,
                    "control renders identically across all " + juce::String (present.size())
                    + " swept states (" + present.joinIntoString ("/") + ") - states appear unstyled" });
            // NO continue: upstream still runs tier 2 on an inert control (it can never fire there -
            // equality tolerance is far tighter than the louder margins - but parity mirrors the flow).
        }

        // ---- tier 2: disabled-not-louder (one-direction, threshold-gated) ----
        const auto n = sampleOf (states.getProperty ("normal", {}));
        const auto d = sampleOf (states.getProperty ("disabled", {}));
        if (! measurable (n) || ! measurable (d)) continue;
        const bool bgOk = (bool) ev.getProperty ("bgIntrospectable", false);
        const auto bgHex = ev.getProperty ("bg", {}).toString();
        if (bgOk && bgHex.startsWithChar ('#')) {
            if (hueDelta (n, d) > kHueSwapDeg && std::abs (d.alpha - n.alpha) <= kAlphaLouderMargin)
                continue;                                        // a colour SWAP, not a dimming failure
            const double cn = contrastRatio (rgbHex (n), bgHex);
            const double cd = contrastRatio (rgbHex (d), bgHex);
            if (cd > cn + kContrastLouderMargin)
                out.push_back ({ "disabled-louder", "low", id,
                    "disabled contrast " + juce::String (cd, 2) + ":1 exceeds normal "
                    + juce::String (cn, 2) + ":1 - disabled reads louder than idle" });
        } else if (d.alpha > n.alpha + kAlphaLouderMargin) {
            out.push_back ({ "disabled-louder", "low", id,
                "disabled alpha " + juce::String (d.alpha, 2) + " exceeds normal alpha "
                + juce::String (n.alpha, 2) + " - disabled reads louder than idle" });
        }
    }
    return out;
}

} // namespace eb::hig

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
double contrastRatio (const juce::String& a, const juce::String& b) {
    const double l1 = lumOf (a), l2 = lumOf (b);
    const double hi = juce::jmax (l1, l2), lo = juce::jmin (l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}
bool isLarge (const El& e) { return e.fontPt >= kLargeFontPt || (e.bold && e.fontPt >= kLargeBoldFontPt); }

} // namespace

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

    return out;
}

} // namespace eb::hig

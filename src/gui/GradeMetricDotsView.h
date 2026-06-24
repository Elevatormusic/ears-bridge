#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/RefMonitor.h"   // eb::QualityBand
#include "gui/Theme.h"

namespace eb {

// A compact 3-dot quality readout: sweepSNR / IR-SNR / THD, each a colored status dot + a short value
// string. Renders alongside the per-ear grade status line. HIG: meaning is NEVER color-alone — every dot
// pairs with a metric LABEL (SNR / IR / THD) and its dB/% VALUE (the number reads on its own); an Unknown
// band shows a HOLLOW dot + an em dash. Semantic Theme colours (ok=green / warn=amber / danger=red /
// textDim=neutral) carry light+dark automatically. Non-interactive (an indicator, not a control), so it sets
// an accessibility description for VoiceOver rather than being a focus target.
class GradeMetricDotsView : public juce::Component {
public:
    GradeMetricDotsView() {
        setInterceptsMouseClicks (false, false);
        setTitle ("Measurement quality");
        clear();
    }

    // Set the three bands + their value strings. Pass an empty value (or QualityBand::Unknown) for "no reading".
    void setMetrics (QualityBand snr, const juce::String& snrVal,
                     QualityBand ir,  const juce::String& irVal,
                     QualityBand thd, const juce::String& thdVal) {
        dots_[0] = { snr, "SNR", snrVal };
        dots_[1] = { ir,  "IR",  irVal  };
        dots_[2] = { thd, "THD", thdVal };
        updateAccessibility();
        repaint();
    }

    void clear() {   // pre-measurement: hollow dots + em dashes
        dots_[0] = { QualityBand::Unknown, "SNR", {} };
        dots_[1] = { QualityBand::Unknown, "IR",  {} };
        dots_[2] = { QualityBand::Unknown, "THD", {} };
        updateAccessibility();
        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto  r      = getLocalBounds().toFloat();
        const float cellW  = r.getWidth() / 3.0f;
        const float dotD   = 8.0f;
        const float cy     = r.getCentreY();
        g.setFont (juce::Font (juce::FontOptions (12.0f)));

        for (int i = 0; i < 3; ++i) {
            const auto& d = dots_[i];
            float x = r.getX() + (float) i * cellW + 2.0f;

            // The status dot: filled for a known band, a hollow outline for Unknown (no reading).
            const juce::Rectangle<float> dot (x, cy - dotD * 0.5f, dotD, dotD);
            if (d.band == QualityBand::Unknown) {
                g.setColour (Theme::textDim());
                g.drawEllipse (dot, 1.2f);
            } else {
                g.setColour (colourFor (d.band));
                g.fillEllipse (dot);
            }
            x += dotD + 6.0f;

            // The label + value text (this is what carries the meaning — never the colour alone).
            const juce::String txt = d.label + " " + (d.value.isNotEmpty() ? d.value : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")));
            g.setColour (Theme::textDim());
            g.drawText (txt, juce::Rectangle<float> (x, r.getY(), cellW - (dotD + 8.0f), r.getHeight()),
                        juce::Justification::centredLeft, true);
        }
    }

private:
    struct Dot { QualityBand band = QualityBand::Unknown; juce::String label, value; };
    Dot dots_[3];

    static juce::Colour colourFor (QualityBand b) {
        switch (b) {
            case QualityBand::Green:  return Theme::okFill();
            case QualityBand::Orange: return Theme::warnFill();
            case QualityBand::Red:    return Theme::dangerFill();
            default:                  return Theme::textDim();
        }
    }
    static const char* bandWord (QualityBand b) {
        switch (b) {
            case QualityBand::Green:  return "good";
            case QualityBand::Orange: return "marginal";
            case QualityBand::Red:    return "poor";
            default:                  return "no reading";
        }
    }
    void updateAccessibility() {   // VoiceOver reads "SNR 28 dB good, IR 60 dB good, THD 0.3% good"
        juce::StringArray parts;
        for (auto& d : dots_)
            parts.add (d.label + " " + (d.value.isNotEmpty() ? d.value : juce::String ("no value"))
                       + " " + bandWord (d.band));
        setDescription (parts.joinIntoString (", "));
    }
};

} // namespace eb

#include <catch2/catch_test_macros.hpp>
#include "gui/stages/CalibrateStage.h"
#include "gui/MainComponent.h"

// The ONE stage caption (spec 5.2, failure-surface map #4/#5). Pure wording rules:
// any empty slot -> the load guidance; else HEQ wins; else HPN; else silent (IDF/RAW/Unknown
// pairs - their cautions live on the cards).
TEST_CASE("CalibrateStage caption: guidance when empty, HEQ once, HPN legacy note, silent for IDF") {
    using CS = eb::CalibrateStage;
    CHECK (CS::stageCaptionFor (std::nullopt, std::nullopt).contains ("HEQ files for headphones"));
    CHECK (CS::stageCaptionFor (eb::CalType::Heq, std::nullopt).contains ("HEQ files for headphones"));
    CHECK (CS::stageCaptionFor (eb::CalType::Heq, eb::CalType::Heq).contains ("bass boost"));
    CHECK (CS::stageCaptionFor (eb::CalType::Heq, eb::CalType::Hpn).contains ("bass boost"));  // HEQ wins
    CHECK (CS::stageCaptionFor (eb::CalType::Hpn, eb::CalType::Hpn).contains ("older curve"));
    CHECK (CS::stageCaptionFor (eb::CalType::Idf, eb::CalType::Idf).isEmpty());   // NEGATIVE: no false caption
}

// The always-visible Advanced-FIR summary: a collapsed disclosure may never hide a non-default
// setting silently - the summary names the current state either way.
TEST_CASE("CalibrateStage advanced summary composes every field") {
    CHECK (eb::CalibrateStage::advancedFirSummary (false, 0, 0.0)
           == "Min phase - Auto length - 0.0 dB trim");
    CHECK (eb::CalibrateStage::advancedFirSummary (true, 16384, -2.0)
           == "Complex phase - 16384 taps - -2.0 dB trim");
}

// P2.9 T7: the frozen open-iff-non-default contract's ONE testable home. The launch seed and the
// harness restore both route through advancedFirNonDefault, so this pure test guards the decision
// both call sites now share - INCLUDING the negative half (all-default -> CLOSED) the disclosure's
// component tests never covered, and the near-zero-trim rows an XML round-trip can produce.
TEST_CASE("P2.9 disclosure seed: open iff any advanced-FIR setting is non-default") {
    // All-default -> CLOSED (the frozen rule's negative half - previously untested).
    CHECK_FALSE (eb::CalibrateStage::advancedFirNonDefault (false, 0, 0.0));
    // Near-zero trim noise (XML round-trip / sub-step values) must still read DEFAULT.
    CHECK_FALSE (eb::CalibrateStage::advancedFirNonDefault (false, 0, -0.0));
    CHECK_FALSE (eb::CalibrateStage::advancedFirNonDefault (false, 0, 1.0e-9));
    // Each knob alone -> OPEN.
    CHECK (eb::CalibrateStage::advancedFirNonDefault (true,  0,     0.0));
    CHECK (eb::CalibrateStage::advancedFirNonDefault (false, 8192,  0.0));
    CHECK (eb::CalibrateStage::advancedFirNonDefault (false, 0,    -6.0));
    CHECK (eb::CalibrateStage::advancedFirNonDefault (false, 0,    -0.1));   // one slider step IS non-default
}

// P2.9 T6: the OUTPUT TRIM control is a parameter row (label left, compact slider + value chip),
// not a full-width debug slider with an all-caps eyebrow stacked above it.
TEST_CASE("P2.9 trim row: label left, compact slider, no full-width debug slider") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    mc.calibrateStageForTest().setAdvancedOpen (true);
    auto& sl = mc.trimSliderForTest();
    auto& lb = mc.trimLabelForTest();
    REQUIRE (sl.isVisible());
    // One 28px row: label and slider vertically aligned, label LEFT of the slider, slider compact.
    CHECK (lb.getBounds().getCentreY() == sl.getBounds().getCentreY());
    CHECK (lb.getRight() <= sl.getX());
    CHECK (sl.getWidth() <= 220);                        // the debug slider spanned the whole column
    CHECK (sl.getHeight() == 28);
    CHECK (lb.getText() == "Output trim");               // eyebrow shouting retired
    mc.calibrateStageForTest().setAdvancedOpen (false);
    tmp.deleteRecursively();
}

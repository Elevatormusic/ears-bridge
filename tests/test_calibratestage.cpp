#include <catch2/catch_test_macros.hpp>
#include "gui/stages/CalibrateStage.h"

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

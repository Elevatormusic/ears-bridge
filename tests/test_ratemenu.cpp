#include <catch2/catch_test_macros.hpp>
#include "gui/RateMenu.h"
#include "audio/CombineMode.h"
#include <vector>

TEST_CASE("buildRateMenu marks every device-native rate as non-resampling") {
    std::vector<double> native { 44100, 48000, 88200, 96000, 176400, 192000 };
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 96000.0);
    REQUIRE (items.size() == 6);
    for (auto& it : items) CHECK (it.resampleWarning == false);
    // The 96 kHz entry is the selected one.
    auto sel = std::find_if (items.begin(), items.end(),
                             [](const eb::RateMenuItem& i){ return i.selected; });
    REQUIRE (sel != items.end());
    CHECK (sel->rate == 96000.0);
}

TEST_CASE("buildRateMenu flags a selected rate the input cannot do natively") {
    std::vector<double> native { 48000 };                 // original EARS
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 96000.0);
    // The 48000 native entry is present and clean...
    REQUIRE (items.size() == 2);
    auto fortyEight = std::find_if (items.begin(), items.end(),
                                    [](const eb::RateMenuItem& i){ return i.rate == 48000.0; });
    REQUIRE (fortyEight != items.end());
    CHECK (fortyEight->resampleWarning == false);
    // ...and the non-native 96000 selection appears, flagged as a resample.
    auto ninetySix = std::find_if (items.begin(), items.end(),
                                   [](const eb::RateMenuItem& i){ return i.rate == 96000.0; });
    REQUIRE (ninetySix != items.end());
    CHECK (ninetySix->resampleWarning == true);
    CHECK (ninetySix->selected == true);
}

TEST_CASE("buildRateMenu without a non-native selection emits exactly the native rates") {
    std::vector<double> native { 48000 };
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 48000.0);
    REQUIRE (items.size() == 1);
    CHECK (items[0].rate == 48000.0);
    CHECK (items[0].resampleWarning == false);
    CHECK (items[0].selected == true);
}

TEST_CASE("combineModeOrder lists the exposed modes with the right recommended badges") {
    auto order = eb::combineModeOrder();
    REQUIRE (order.size() == 3);   // TwoPass L/R were removed from the UI (superseded by AutoPerEar)
    CHECK (order[0].mode == eb::CombineMode::Average);
    CHECK (order[1].mode == eb::CombineMode::Sum);
    CHECK (order[2].mode == eb::CombineMode::AutoPerEar);
    // Only AutoPerEar carries the Recommended badge — it is the one recommended Dirac headphone mode.
    CHECK (order[0].recommended == false);   // Average
    CHECK (order[1].recommended == false);   // Sum
    CHECK (order[2].recommended == true);    // AutoPerEar
    // Sum carries the +6 dB clip-risk warning.
    CHECK (order[1].clipRiskWarning == true);
}

TEST_CASE("numTapsForRate scales 8192 @ 48k to powers of two") {
    CHECK (eb::numTapsForRate (48000.0)  == 8192);
    CHECK (eb::numTapsForRate (96000.0)  == 16384);
    CHECK (eb::numTapsForRate (192000.0) == 32768);
    CHECK (eb::numTapsForRate (44100.0)  == 8192);   // ~7526 -> nearest pow2 = 8192
    CHECK (eb::numTapsForRate (88200.0)  == 16384);  // ~15053 -> nearest pow2 = 16384
    CHECK (eb::numTapsForRate (176400.0) == 32768);  // ~30106 -> nearest pow2 = 32768
}

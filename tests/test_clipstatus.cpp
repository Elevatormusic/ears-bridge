#include <catch2/catch_test_macros.hpp>
#include "gui/ClipStatus.h"
#include <string>

TEST_CASE("invalidMeasurementMessage names the most specific cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::NonFinite))
           == "Measurement invalidated by a corrupted audio sample.");
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::Dropout))
           == "Dropouts detected - this measurement is invalid.");
    // Confirmed clip takes precedence over a co-occurring dropout (most actionable for the user).
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::Dropout))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
}

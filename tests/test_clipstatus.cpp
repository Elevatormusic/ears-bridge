#include <catch2/catch_test_macros.hpp>
#include "gui/ClipStatus.h"
#include <string>

TEST_CASE("invalidMeasurementMessage names the most specific cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::NonFinite).toStdString()
           == "Measurement invalidated by a corrupted audio sample.");
    CHECK (invalidMeasurementMessage (HealthFlag::Dropout).toStdString()
           == "Dropouts detected - this measurement is invalid.");
    // Confirmed clip takes precedence over a co-occurring dropout (most actionable for the user).
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::Dropout).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::NonFinite | HealthFlag::Dropout).toStdString()
           == "Measurement invalidated by a corrupted audio sample.");
}

TEST_CASE("invalidMeasurementMessage names clock drift distinctly from dropouts") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::ExcessDrift).toStdString()
           == "Sample-clock drift detected - this measurement is invalid.");
    CHECK (invalidMeasurementMessage (HealthFlag::Xrun).toStdString()
           == "Dropouts detected - this measurement is invalid.");
    // Clip still outranks drift.
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::ExcessDrift).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
}

TEST_CASE("invalidMeasurementMessage qualifies a confirmed clip as approximate when OS-resampled") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::OsResampled).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat. "
              "(OS-resampled - approximate)");
    CHECK (invalidMeasurementMessage (HealthFlag::Dropout | HealthFlag::OsResampled).toStdString()
           == "Dropouts detected - this measurement is invalid.");
    CHECK (invalidMeasurementMessage (HealthFlag::ExcessDrift).toStdString()       // drift branch survives the migration
           == "Sample-clock drift detected - this measurement is invalid.");
}

TEST_CASE("invalidMeasurementMessage: SweepRetimed names the clock-retiming cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed).containsIgnoreCase ("retim"));
    // ClipConfirmed and NonFinite still take precedence.
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::SweepRetimed)
               .containsIgnoreCase ("full scale"));
    // SweepRetimed outranks the generic ExcessDrift wording.
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed | HealthFlag::ExcessDrift).toStdString()
               == "Clock drift retimed the sweep - this measurement is invalid. Repeat the measurement.");
}

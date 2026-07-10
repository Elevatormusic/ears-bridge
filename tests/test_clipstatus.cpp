#include <catch2/catch_test_macros.hpp>
#include "gui/ClipStatus.h"
#include "gui/Copy.h"

TEST_CASE("invalidMeasurementMessage names the most specific cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage; using eb::kDash;
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed)
           == "Input reached digital full scale" + kDash + "this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::NonFinite).toStdString()
           == "Measurement invalidated by a corrupted audio sample.");
    CHECK (invalidMeasurementMessage (HealthFlag::Dropout)
           == "Dropouts detected" + kDash + "this measurement is invalid.");
    // Confirmed clip takes precedence over a co-occurring dropout (most actionable for the user).
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::Dropout)
           == "Input reached digital full scale" + kDash + "this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::NonFinite | HealthFlag::Dropout).toStdString()
           == "Measurement invalidated by a corrupted audio sample.");
}

TEST_CASE("invalidMeasurementMessage names clock drift distinctly from dropouts") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage; using eb::kDash;
    CHECK (invalidMeasurementMessage (HealthFlag::ExcessDrift)
           == "Sample-clock drift detected" + kDash + "this measurement is invalid.");
    CHECK (invalidMeasurementMessage (HealthFlag::Xrun)
           == "Dropouts detected" + kDash + "this measurement is invalid.");
    // Clip still outranks drift.
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::ExcessDrift)
           == "Input reached digital full scale" + kDash + "this measurement is invalid. Lower the level and repeat.");
}

TEST_CASE("invalidMeasurementMessage qualifies a confirmed clip as approximate when OS-resampled") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage; using eb::kDash;
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed)
           == "Input reached digital full scale" + kDash + "this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::OsResampled)
           == "Input reached digital full scale" + kDash + "this measurement is invalid. Lower the level and repeat. "
              "(OS-resampled" + kDash + "approximate)");
    CHECK (invalidMeasurementMessage (HealthFlag::Dropout | HealthFlag::OsResampled)
           == "Dropouts detected" + kDash + "this measurement is invalid.");
    CHECK (invalidMeasurementMessage (HealthFlag::ExcessDrift)                      // drift branch survives the migration
           == "Sample-clock drift detected" + kDash + "this measurement is invalid.");
}

TEST_CASE("invalidMeasurementMessage: SweepRetimed names the clock-retiming cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage; using eb::kDash;
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed).containsIgnoreCase ("retim"));
    // ClipConfirmed and NonFinite still take precedence.
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::SweepRetimed)
               .containsIgnoreCase ("full scale"));
    // SweepRetimed outranks the generic ExcessDrift wording.
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed | HealthFlag::ExcessDrift)
               == "Clock drift retimed the sweep" + kDash + "this measurement is invalid. Repeat the measurement.");
}

TEST_CASE("invalidMeasurementMessage: FormatChanged names the device-format cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage; using eb::kDash;
    CHECK (invalidMeasurementMessage(HealthFlag::FormatChanged)
           == "Audio device format changed mid-run" + kDash + "this measurement is invalid. Prevent sleep/wake during capture.");
    CHECK (invalidMeasurementMessage(HealthFlag::ClipConfirmed | HealthFlag::FormatChanged).containsIgnoreCase("full scale"));
    CHECK (invalidMeasurementMessage(HealthFlag::NonFinite | HealthFlag::FormatChanged).containsIgnoreCase("corrupted"));
    CHECK (invalidMeasurementMessage(HealthFlag::SweepRetimed | HealthFlag::FormatChanged).containsIgnoreCase("retim"));  // SweepRetimed wins
    CHECK (invalidMeasurementMessage(HealthFlag::ExcessDrift | HealthFlag::FormatChanged).containsIgnoreCase("format"));  // FormatChanged wins
    CHECK (invalidMeasurementMessage(HealthFlag::Dropout | HealthFlag::FormatChanged).containsIgnoreCase("format"));      // FormatChanged wins
}

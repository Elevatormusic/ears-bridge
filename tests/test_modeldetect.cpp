#include <catch2/catch_test_macros.hpp>
#include "audio/ModelDetect.h"
using eb::EarsModel;

TEST_CASE("detectEarsModel: EARS Pro recognised before plain EARS") {
    CHECK (eb::detectEarsModel ("EARS Pro")            == EarsModel::EarsPro);
    CHECK (eb::detectEarsModel ("miniDSP EARS Pro")    == EarsModel::EarsPro);
    CHECK (eb::detectEarsModel ("MINIDSP EARSPRO USB") == EarsModel::EarsPro);
    CHECK (eb::detectEarsModel ("Microphone (E.A.R.S Pro)") == EarsModel::EarsPro);  // dotted driver name
}

TEST_CASE("detectEarsModel: original EARS") {
    CHECK (eb::detectEarsModel ("EARS")        == EarsModel::Ears);
    CHECK (eb::detectEarsModel ("miniDSP EARS") == EarsModel::Ears);
    CHECK (eb::detectEarsModel ("Microphone (EARS)") == EarsModel::Ears);
    // Real Windows endpoint name for the original EARS: the driver dots the letters.
    CHECK (eb::detectEarsModel ("Microphone (E.A.R.S Gain: 18dB)") == EarsModel::Ears);
}

TEST_CASE("detectEarsModel: USB VID:PID promotes a generic name") {
    // miniDSP VID 2752; EARS Pro XMOS interface reported with a bare name.
    CHECK (eb::detectEarsModel ("USB Audio Device", "VID_2752&PID_0046") == EarsModel::EarsPro);
    CHECK (eb::detectEarsModel ("USB Audio Device", "VID_2752&PID_0011") == EarsModel::Ears);
}

TEST_CASE("detectEarsModel: unrelated devices are Unknown") {
    CHECK (eb::detectEarsModel ("Realtek High Definition Audio") == EarsModel::Unknown);
    CHECK (eb::detectEarsModel ("CABLE Output (VB-Audio Virtual Cable)") == EarsModel::Unknown);
}

TEST_CASE("native rate/bit-depth whitelists per model") {
    CHECK (eb::nativeSampleRates (EarsModel::Ears)    == std::vector<double>{48000});
    CHECK (eb::nativeSampleRates (EarsModel::EarsPro) ==
           std::vector<double>{44100,48000,88200,96000,176400,192000});
    CHECK (eb::nativeBitDepths (EarsModel::Ears)    == std::vector<int>{24});
    CHECK (eb::nativeBitDepths (EarsModel::EarsPro) == std::vector<int>{16,24,32});
    CHECK (eb::nativeSampleRates (EarsModel::Unknown).empty());
    CHECK (eb::nativeBitDepths   (EarsModel::Unknown).empty());
}

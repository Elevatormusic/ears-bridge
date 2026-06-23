// test_startgate.cpp
// Unit-tests for the pure Start-gate predicate eb::startReady (Task 3 / #3).
// The advanced override relaxes ONLY the two policy gates (wrongMode, physicalOutput);
// it must NEVER bypass the device-present or calibration-applied requirements.
#include <catch2/catch_test_macros.hpp>
#include "gui/StartGate.h"

// --- override == false: today's behaviour exactly ---------------------------

TEST_CASE("startReady: no override, everything good -> ready") {
    CHECK (eb::startReady (/*haveDevs*/true, /*haveCals*/true,
                           /*wrongMode*/false, /*physicalOutput*/false, /*override*/false));
}

TEST_CASE("startReady: no override, wrongMode blocks") {
    CHECK_FALSE (eb::startReady (true, true, /*wrongMode*/true, false, /*override*/false));
}

TEST_CASE("startReady: no override, physicalOutput blocks") {
    CHECK_FALSE (eb::startReady (true, true, false, /*physicalOutput*/true, /*override*/false));
}

TEST_CASE("startReady: no override, missing devices blocks") {
    CHECK_FALSE (eb::startReady (/*haveDevs*/false, true, false, false, /*override*/false));
}

TEST_CASE("startReady: no override, missing calibration blocks") {
    CHECK_FALSE (eb::startReady (true, /*haveCals*/false, false, false, /*override*/false));
}

// --- override == true: relaxes ONLY the two policy gates ---------------------

TEST_CASE("startReady: override unlocks BOTH policy gates when devs+cals present") {
    // The headline case: wrongMode AND physicalOutput both set, but with devices and
    // calibration present and the override on -> ready for a non-Dirac use case.
    CHECK (eb::startReady (/*haveDevs*/true, /*haveCals*/true,
                           /*wrongMode*/true, /*physicalOutput*/true, /*override*/true));
}

TEST_CASE("startReady: override + wrongMode only -> ready") {
    CHECK (eb::startReady (true, true, /*wrongMode*/true, /*physicalOutput*/false, /*override*/true));
}

TEST_CASE("startReady: override + physicalOutput only -> ready") {
    CHECK (eb::startReady (true, true, /*wrongMode*/false, /*physicalOutput*/true, /*override*/true));
}

// --- load-bearing SAFETY assertions: override can NEVER bypass devs or cals ---

TEST_CASE("startReady: override can NOT bypass missing calibration") {
    // Even with the override on and both policy gates relaxed, an unapplied
    // calibration must keep Start blocked -- this is the atomic correctness gate.
    CHECK_FALSE (eb::startReady (/*haveDevs*/true, /*haveCals*/false,
                                 /*wrongMode*/true, /*physicalOutput*/true, /*override*/true));
}

TEST_CASE("startReady: override can NOT bypass missing devices") {
    // Likewise no input/output device selected must keep Start blocked regardless
    // of the override -- you cannot run the engine without devices.
    CHECK_FALSE (eb::startReady (/*haveDevs*/false, /*haveCals*/true,
                                 /*wrongMode*/true, /*physicalOutput*/true, /*override*/true));
}

TEST_CASE("startReady: override can NOT bypass BOTH missing devs and cals") {
    CHECK_FALSE (eb::startReady (/*haveDevs*/false, /*haveCals*/false,
                                 /*wrongMode*/false, /*physicalOutput*/false, /*override*/true));
}

// --- noCalsLoaded: no cal file at all -> unity passthrough, Start allowed (uncalibrated) ------------

TEST_CASE("startReady: NO cals loaded (unity passthrough) -> ready even though haveCals is false") {
    // The user's case: removed both cals to measure uncalibrated. Engine runs a neutral unity FIR.
    CHECK (eb::startReady (/*haveDevs*/true, /*haveCals*/false, /*wrongMode*/false,
                           /*physicalOutput*/false, /*override*/false, /*noCalsLoaded*/true));
}

TEST_CASE("startReady: a cal LOADED but not yet applied (half-built) still BLOCKS") {
    // haveCals=false because the generation isn't applied yet, AND noCalsLoaded=false because a file IS
    // loaded -> blocked (a half-built/stale cal would corrupt capture). This is the protection we keep.
    CHECK_FALSE (eb::startReady (true, /*haveCals*/false, false, false, /*override*/false,
                                 /*noCalsLoaded*/false));
}

TEST_CASE("startReady: NO cals loaded still needs devices") {
    CHECK_FALSE (eb::startReady (/*haveDevs*/false, false, false, false, false, /*noCalsLoaded*/true));
}

TEST_CASE("startReady: NO cals loaded still respects the policy gates without override") {
    CHECK_FALSE (eb::startReady (true, false, /*wrongMode*/true, false, /*override*/false, /*noCalsLoaded*/true));
    CHECK_FALSE (eb::startReady (true, false, false, /*physicalOutput*/true, /*override*/false, /*noCalsLoaded*/true));
}

TEST_CASE("startReady: NO cals loaded + override relaxes the policy gates") {
    CHECK (eb::startReady (true, /*haveCals*/false, /*wrongMode*/true, /*physicalOutput*/true,
                           /*override*/true, /*noCalsLoaded*/true));
}

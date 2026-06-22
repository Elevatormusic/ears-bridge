#include <catch2/catch_test_macros.hpp>
#include "gui/SignalChainView.h"

using eb::ChainRow;
using eb::ChainRowState;
using eb::chainRow;
using eb::signalChainRows;

namespace {
eb::EndpointFormat fmt (bool valid, double rate, int ch, int bits, bool isFloat) {
    eb::EndpointFormat f;
    f.valid = valid; f.mixRateHz = rate; f.channels = ch; f.bits = bits; f.isFloat = isFloat;
    return f;
}
}

TEST_CASE("chainRow: a clean 48k float endpoint reads Good with rate + detail") {
    const auto r = chainRow ("EARS input", fmt (true, 48000.0, 2, 32, true));
    CHECK (r.name == "EARS input");
    CHECK (r.rate == "48 kHz");
    CHECK (r.state == ChainRowState::Good);
    CHECK (r.detail == "2 ch, 32-bit float");
}

TEST_CASE("chainRow: an off-48k endpoint reads Warn and names the actual rate (the 44.1k trap)") {
    const auto r = chainRow ("Dirac output", fmt (true, 44100.0, 2, 24, false));
    CHECK (r.rate == "44.1 kHz");
    CHECK (r.state == ChainRowState::Warn);
    CHECK (r.detail == "2 ch, 24-bit");
}

TEST_CASE("chainRow: an unreadable endpoint reads Unknown, never a silent pass") {
    const auto r = chainRow ("Virtual cable", fmt (false, 0.0, 0, 0, false));
    CHECK (r.state == ChainRowState::Unknown);
    CHECK (r.rate == "?");
    CHECK (r.detail == "couldn't read");
}

TEST_CASE("chainRow: a mono input is flagged in the detail (rate is still the only gate)") {
    const auto r = chainRow ("EARS input", fmt (true, 48000.0, 1, 16, false));
    CHECK (r.state == ChainRowState::Good);          // channels are advisory, not the rate gate
    CHECK (r.detail == "1 ch (mono), 16-bit");
}

TEST_CASE("chainRow: a 96k endpoint reads Warn with a whole-number kHz label") {
    const auto r = chainRow ("Dirac output", fmt (true, 96000.0, 2, 24, true));
    CHECK (r.rate == "96 kHz");
    CHECK (r.state == ChainRowState::Warn);
}

TEST_CASE("signalChainRows: three rows in fixed chain order") {
    eb::ChainConfig c;
    c.input       = fmt (true, 48000.0, 2, 24, false);
    c.cable       = fmt (true, 48000.0, 2, 24, false);
    c.diracOutput = fmt (true, 44100.0, 2, 24, false);   // the off-rate one
    const auto rows = signalChainRows (c);
    CHECK (rows[0].name == "EARS input");
    CHECK (rows[1].name == "Virtual cable");
    CHECK (rows[2].name == "Dirac output");
    CHECK (rows[0].state == ChainRowState::Good);
    CHECK (rows[1].state == ChainRowState::Good);
    CHECK (rows[2].state == ChainRowState::Warn);
}

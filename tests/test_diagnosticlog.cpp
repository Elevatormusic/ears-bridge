#include <catch2/catch_test_macros.hpp>
#include "diag/DiagnosticLog.h"

// Rotating, message-thread diagnostic logger (Task 1 of the match-detection +
// diagnostic-logging build). Three things are load-bearing and tested here:
//   1. REDACTION  — the EARS serial must NEVER survive into a log line, in either
//      its dashed ("000-0000") or dash-less ("0000000") form. No real serial is
//      used; "000-0000" is a synthetic placeholder.
//   2. ROTATION   — once a file reaches kMaxBytes it rolls eb.log -> eb.1.log ->
//      eb.2.log and deletes the oldest beyond kMaxBackups, so the logs dir holds
//      at most kMaxBackups+1 files and none grows without bound.
//   3. WRITE      — a line lands in currentFile() with a "[HH:MM:SS.mmm] [LVL] "
//      timestamp + level prefix.
// Everything is synthetic — a unique temp subdir, cleaned up after each case.

using eb::DiagnosticLog;

namespace {
// A unique scratch logs dir under %TEMP% so concurrent runs / the real app's logs
// can never collide. Mirrors tests/test_settings.cpp::makeTempDir().
juce::File makeTempDir() {
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("eb_diaglog_test_" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}
} // namespace

TEST_CASE("DiagnosticLog::redactSerial scrubs the dashed serial, keeps the rest") {
    const auto out = DiagnosticLog::redactSerial ("loaded R_HPN serial 000-0000 ok", "000-0000");
    CHECK (out.contains ("[serial redacted]"));
    CHECK_FALSE (out.contains ("000-0000"));   // the value must NOT survive
    CHECK (out.contains ("loaded R_HPN serial"));   // surrounding context is preserved
    CHECK (out.contains ("ok"));
}

TEST_CASE("DiagnosticLog::redactSerial also scrubs the dash-less form of the serial") {
    // A caller may have stripped the dash before logging; the dash-less digits must
    // still be redacted so the value can't leak in either shape.
    const auto out = DiagnosticLog::redactSerial ("device id 0000000 connected", "000-0000");
    CHECK (out.contains ("[serial redacted]"));
    CHECK_FALSE (out.contains ("0000000"));
}

TEST_CASE("DiagnosticLog::redactSerial with an empty serial is a no-op") {
    const juce::String msg = "no serial known here";
    CHECK (DiagnosticLog::redactSerial (msg, juce::String()) == msg);
}

TEST_CASE("DiagnosticLog writes a timestamped, level-prefixed line") {
    auto dir = makeTempDir();
    {
        DiagnosticLog log (dir);
        log.write (DiagnosticLog::Level::Info,  "first line");
        log.write (DiagnosticLog::Level::Error, "second line");

        CHECK (log.directory()   == dir);
        CHECK (log.currentFile() == dir.getChildFile ("eb.log"));
    }   // stream closes on destruction so we can read the file back

    const auto text = dir.getChildFile ("eb.log").loadFileAsString();
    auto lines = juce::StringArray::fromLines (text.trimEnd());
    CHECK (lines.size() == 2);

    // "[HH:MM:SS.mmm] [LVL] message" — bracketed timestamp, then a bracketed level, then text.
    CHECK (lines[0].startsWith ("["));
    CHECK (lines[0].contains ("[INFO]"));
    CHECK (lines[0].endsWith ("first line"));
    CHECK (lines[1].contains ("[ERROR]"));
    CHECK (lines[1].endsWith ("second line"));

    dir.deleteRecursively();
}

TEST_CASE("DiagnosticLog rotation caps the file count and size") {
    auto dir = makeTempDir();

    // One ~64 KB line; enough lines to overflow kMaxBytes several times so we exercise
    // both the eb.log->eb.1.log->eb.2.log shift AND the delete-oldest-beyond-cap path.
    const juce::String bigLine (juce::String::repeatedString ("x", 64 * 1024));
    const auto linesToOverflow = (int) (DiagnosticLog::kMaxBytes / bigLine.length()) + 1;
    {
        DiagnosticLog log (dir);
        for (int i = 0; i < linesToOverflow * (DiagnosticLog::kMaxBackups + 2); ++i)
            log.write (DiagnosticLog::Level::Info, bigLine);
    }

    // At most kMaxBackups+1 log files survive (eb.log + eb.1.log..eb.kMaxBackups.log).
    auto logs = dir.findChildFiles (juce::File::findFiles, false, "eb*.log");
    CHECK (logs.size() <= DiagnosticLog::kMaxBackups + 1);

    // Rotation actually happened: the first backup exists.
    CHECK (dir.getChildFile ("eb.1.log").existsAsFile());

    // No file blew past the cap (allow one over-cap line, since we rotate-then-write).
    for (const auto& f : logs)
        CHECK (f.getSize() <= DiagnosticLog::kMaxBytes + bigLine.length() + 8);

    dir.deleteRecursively();
}

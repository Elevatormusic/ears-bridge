#include <catch2/catch_test_macros.hpp>
#include "diag/DiagnosticLog.h"

// Per-launch, message-thread diagnostic logger (Task 1 of the match-detection +
// diagnostic-logging build). Four things are load-bearing and tested here:
//   1. REDACTION    — the EARS serial must NEVER survive into a log line, in either
//      its dashed ("000-0000") or dash-less ("0000000") form. No real serial is
//      used; "000-0000" is a synthetic placeholder.
//   2. PER-LAUNCH   — two DiagnosticLog instances pointed at the SAME dir write to
//      DIFFERENT files and BOTH open successfully (the overlapping-instance bug:
//      before, the second instance shared one "eb.log" handle and silently logged
//      nothing). This is the core fix.
//   3. FOLDER CAP   — across many launches the folder is pruned oldest-first to at
//      most kMaxFiles AND the kMaxFolderBytes size budget; it can never grow without
//      bound.
//   4. WITHIN-FILE  — a single launch's file is still size-capped: once it reaches
//      kMaxBytes it rolls to "<base>.1" so no one file grows without bound.
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

// All session log files in the dir (the per-launch "eb-*.log" + any ".N" overflow rolls).
juce::Array<juce::File> sessionFiles (const juce::File& dir) {
    return dir.findChildFiles (juce::File::findFiles, false, "eb-*.log*");
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

TEST_CASE("DiagnosticLog writes a timestamped, level-prefixed line to a per-launch file") {
    auto dir = makeTempDir();
    juce::File written;
    {
        DiagnosticLog log (dir);
        log.write (DiagnosticLog::Level::Info,  "first line");
        log.write (DiagnosticLog::Level::Error, "second line");

        CHECK (log.directory() == dir);
        // This launch's file lives in the dir and follows the eb-<stamp>.log scheme.
        written = log.currentFile();
        CHECK (written.getParentDirectory() == dir);
        CHECK (written.getFileName().startsWith ("eb-"));
        CHECK (written.getFileName().endsWith (".log"));
    }   // stream closes on destruction so we can read the file back

    const auto text = written.loadFileAsString();
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

TEST_CASE("DiagnosticLog: two overlapping instances on one dir use DIFFERENT files, both succeed") {
    // THE FIX. Before, both instances opened a single shared "eb.log"; the second
    // instance's stream failed to open and silently logged nothing. Now each launch
    // owns its own uniquely-named file, so an overlapping (still-open) instance can't
    // starve a relaunch.
    auto dir = makeTempDir();
    {
        DiagnosticLog a (dir);   // first launch, still holding its file...
        DiagnosticLog b (dir);   // ...second launch opens while a is alive

        // Different files (the whole point), and neither is empty after writing.
        CHECK (a.currentFile() != b.currentFile());

        a.write (DiagnosticLog::Level::Info, "launch A banner");
        b.write (DiagnosticLog::Level::Info, "launch B banner");

        CHECK (a.currentFile().existsAsFile());
        CHECK (b.currentFile().existsAsFile());
    }   // both streams close so we can read both back

    // BOTH files exist and BOTH carry their own banner (the second one is NOT empty).
    int nonEmpty = 0;
    for (const auto& f : sessionFiles (dir))
        if (f.getSize() > 0) ++nonEmpty;
    CHECK (nonEmpty == 2);

    dir.deleteRecursively();
}

TEST_CASE("DiagnosticLog: the folder is pruned to at most kMaxFiles, newest kept") {
    auto dir = makeTempDir();

    // Construct many launches in sequence (each creates its own file). Because they
    // can land in the same wall-clock second, the uniquifier keeps them distinct.
    // Far more than kMaxFiles, so pruning MUST kick in.
    const int launches = DiagnosticLog::kMaxFiles * 3;
    for (int i = 0; i < launches; ++i) {
        DiagnosticLog log (dir);
        log.write (DiagnosticLog::Level::Info, "launch " + juce::String (i));
    }

    auto files = sessionFiles (dir);
    CHECK (files.size() <= DiagnosticLog::kMaxFiles);
    CHECK (files.size() >= 1);   // we did keep something

    dir.deleteRecursively();
}

TEST_CASE("DiagnosticLog: the folder honours the total-size budget") {
    auto dir = makeTempDir();

    // Each launch writes a ~3 MB file. kMaxFolderBytes is ~15 MB, so well under
    // kMaxFiles (8) launches' worth, the SIZE budget — not the count — must bound it.
    const juce::String bigLine (juce::String::repeatedString ("y", 256 * 1024));
    const int linesPerLaunch = (int) (3 * 1024 * 1024 / bigLine.length());
    const int launches = (int) (DiagnosticLog::kMaxFolderBytes / (3 * 1024 * 1024)) + 6;
    for (int i = 0; i < launches; ++i) {
        DiagnosticLog log (dir);
        for (int j = 0; j < linesPerLaunch; ++j)
            log.write (DiagnosticLog::Level::Info, bigLine);
    }

    juce::int64 total = 0;
    for (const auto& f : sessionFiles (dir))
        total += f.getSize();
    // Pruning runs at construction (before the new file opens), so the folder stays
    // within budget plus at most one freshly-growing file's worth of slack.
    CHECK (total <= DiagnosticLog::kMaxFolderBytes + 3 * 1024 * 1024 + bigLine.length());

    dir.deleteRecursively();
}

TEST_CASE("DiagnosticLog: a single launch's file is still size-capped (rolls to <base>.1)") {
    auto dir = makeTempDir();

    // One ~64 KB line; enough lines to overflow kMaxBytes so we exercise the
    // within-file roll for THIS launch's base file.
    const juce::String bigLine (juce::String::repeatedString ("x", 64 * 1024));
    const auto linesToOverflow = (int) (DiagnosticLog::kMaxBytes / bigLine.length()) + 1;
    juce::File base;
    {
        DiagnosticLog log (dir);
        base = log.currentFile();
        for (int i = 0; i < linesToOverflow + 4; ++i)
            log.write (DiagnosticLog::Level::Info, bigLine);
    }

    // The roll happened: this launch's "<base>.1" exists, and no file blew past the
    // cap (allow one over-cap line, since we rotate-then-write).
    const auto roll = base.getSiblingFile (base.getFileName() + ".1");
    CHECK (roll.existsAsFile());
    for (const auto& f : sessionFiles (dir))
        CHECK (f.getSize() <= DiagnosticLog::kMaxBytes + bigLine.length() + 8);

    dir.deleteRecursively();
}

TEST_CASE("DiagnosticLog: a written line is scrubbed of the serial on disk") {
    // End-to-end redaction: a serial handed to write() must NOT survive into the file,
    // in either form. (Callers run redactSerial before write; this guards the on-disk
    // result directly.)
    auto dir = makeTempDir();
    juce::File written;
    {
        DiagnosticLog log (dir);
        written = log.currentFile();
        log.write (DiagnosticLog::Level::Info,
                   DiagnosticLog::redactSerial ("device serial 000-0000 / 0000000 seen", "000-0000"));
    }
    const auto text = written.loadFileAsString();
    CHECK (text.contains ("[serial redacted]"));
    CHECK_FALSE (text.contains ("000-0000"));
    CHECK_FALSE (text.contains ("0000000"));

    dir.deleteRecursively();
}

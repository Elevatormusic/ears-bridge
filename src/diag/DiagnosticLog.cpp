#include "diag/DiagnosticLog.h"

namespace eb {

namespace {

// Per-launch base name "eb-<YYYYMMDD-HHMMSS>.log". A separate uniquifier (0 = none,
// >0 = "-N") disambiguates two launches in the same wall-clock second so they never
// share a handle. The ".log" extension keeps the existing "eb*.log" glob + the
// "Open log folder" / "Export log" affordances working unchanged.
juce::String launchBaseName (const juce::Time& now, int uniquifier) {
    juce::String name = "eb-" + now.formatted ("%Y%m%d-%H%M%S");
    if (uniquifier > 0)
        name += "-" + juce::String (uniquifier);
    return name + ".log";
}

// Per-launch rotation backups: "<base>" is the live file (e.g. eb-20260621-101530.log),
// "<base>.N" (N = 1..kMaxBackups) are this launch's overflow rolls.
juce::File backupOf (const juce::File& base, int backupIndex) {
    return backupIndex <= 0 ? base
                            : base.getSiblingFile (base.getFileName() + "." + juce::String (backupIndex));
}

const char* levelTag (DiagnosticLog::Level level) {
    switch (level) {
        case DiagnosticLog::Level::Debug: return "DBG";
        case DiagnosticLog::Level::Info:  return "INFO";
        case DiagnosticLog::Level::Warn:  return "WARN";
        case DiagnosticLog::Level::Error: return "ERROR";
    }
    return "INFO";
}

// "[HH:MM:SS.mmm]" wall-clock stamp. Local time is what a user reads off the clock
// when correlating a log against what they saw on screen.
juce::String timestamp() {
    const auto now = juce::Time::getCurrentTime();
    return "[" + now.formatted ("%H:%M:%S")
         + "." + juce::String (now.getMilliseconds()).paddedLeft ('0', 3) + "]";
}

// All log files this logger family owns, in OLDEST-first order. Sorting by name works
// because the "eb-<YYYYMMDD-HHMMSS>" prefix sorts chronologically; the same-second
// "-N" uniquifier and the ".N" overflow suffix both sort after the bare file, which
// is fine for "delete oldest first" pruning (we only need a stable oldest-first order).
juce::Array<juce::File> sessionFilesOldestFirst (const juce::File& dir) {
    auto files = dir.findChildFiles (juce::File::findFiles, false, "eb-*.log*");
    files.sort();   // ascending by full path -> oldest timestamp first
    return files;
}

} // namespace

DiagnosticLog::DiagnosticLog (const juce::File& dir)
    : dir_ (dir) {
    dir_.createDirectory();   // no-op if it already exists

    // Bound the folder FIRST (before adding this launch's file), so the new file is
    // never a pruning candidate and we don't briefly exceed the budget.
    pruneFolder();

    // Open a fresh, uniquely-named file for THIS launch. Two overlapping instances
    // (kill/relaunch race, or two open at once) get different names and so never
    // contend for one handle. If a name can't be opened, advance the uniquifier and
    // retry rather than silently logging nothing.
    const auto now = juce::Time::getCurrentTime();
    for (int uniquifier = 0; uniquifier < 1000; ++uniquifier) {
        auto candidate = dir_.getChildFile (launchBaseName (now, uniquifier));
        if (candidate.existsAsFile())
            continue;   // another in-the-same-second launch already took this name
        auto stream = std::make_unique<juce::FileOutputStream> (candidate);
        if (stream->openedOk()) {
            current_ = candidate;
            out_     = std::move (stream);
            return;
        }
        // Couldn't open (locked / transient) -> try the next uniquifier.
    }
    // Exhausted (pathological); leave out_ null. write() degrades to a no-op, and
    // currentFile() still returns a sensible per-launch name for the GUI.
    current_ = dir_.getChildFile (launchBaseName (now, 0));
}

juce::File DiagnosticLog::directory()   const { return dir_; }
juce::File DiagnosticLog::currentFile() const { return current_; }

void DiagnosticLog::write (Level level, const juce::String& message) {
    rotateIfNeeded();
    if (out_ == nullptr || ! out_->openedOk())
        return;   // can't open the file (permissions, full disk) -> drop silently; logging must never throw

    out_->writeText (timestamp() + " [" + levelTag (level) + "] " + message + "\n",
                     /*asUTF16*/ false, /*byteOrderMark*/ false, /*lineFeed*/ nullptr);
    out_->flush();   // a crash mid-run must still leave the line on disk
}

void DiagnosticLog::pruneFolder() const {
    auto files = sessionFilesOldestFirst (dir_);

    // Reserve one slot for THIS launch's file (opened right after pruning), so the
    // folder holds at most kMaxFiles AFTER the new file lands.
    const int keepCount = juce::jmax (0, kMaxFiles - 1);

    // Running total of the bytes we'd keep, summed from NEWEST backwards, so we drop
    // the oldest first to satisfy both the count budget and the size budget.
    juce::int64 keptBytes = 0;
    for (int i = files.size() - 1; i >= 0; --i) {
        const int keptSoFar = files.size() - 1 - i;   // how many newer files we've already kept
        const auto size = files[i].getSize();
        const bool overCount = keptSoFar >= keepCount;
        const bool overSize  = (keptBytes + size) > kMaxFolderBytes && keptSoFar > 0;
        if (overCount || overSize)
            files[i].deleteFile();   // oldest-first delete
        else
            keptBytes += size;
    }
}

void DiagnosticLog::rotateIfNeeded() {
    if (out_ == nullptr || ! out_->openedOk())
        return;
    if (out_->getPosition() < kMaxBytes)
        return;

    // Close the live stream before we move files around underneath it.
    out_.reset();

    // Drop the oldest backup, then shift each remaining backup up one slot:
    // <base>.(N) -> <base>.(N+1), oldest first so we never overwrite a file we
    // still need.
    backupOf (current_, kMaxBackups).deleteFile();
    for (int i = kMaxBackups - 1; i >= 1; --i)
        backupOf (current_, i).moveFileTo (backupOf (current_, i + 1));

    // <base> becomes <base>.1, then reopen a fresh, empty <base>.
    current_.moveFileTo (backupOf (current_, 1));
    out_ = std::make_unique<juce::FileOutputStream> (current_);
}

juce::String DiagnosticLog::redactSerial (const juce::String& message, const juce::String& serial) {
    if (serial.isEmpty())
        return message;

    const juce::String marker ("[serial redacted]");
    auto out = message.replace (serial, marker);

    // Also scrub the dash-less digits, in case a caller stripped the dash before
    // logging (e.g. "000-0000" logged as "0000000"). Only when it differs, so we
    // don't double-scan an already-dashless serial.
    const auto bare = serial.removeCharacters ("-");
    if (bare.isNotEmpty() && bare != serial)
        out = out.replace (bare, marker);

    return out;
}

} // namespace eb

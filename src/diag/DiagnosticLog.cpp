#include "diag/DiagnosticLog.h"

namespace eb {

namespace {

// "eb.log" is the live file; "eb.N.log" (N = 1..kMaxBackups) are the backups.
juce::File logFile (const juce::File& dir, int backupIndex) {
    return backupIndex <= 0 ? dir.getChildFile ("eb.log")
                            : dir.getChildFile ("eb." + juce::String (backupIndex) + ".log");
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

} // namespace

DiagnosticLog::DiagnosticLog (const juce::File& dir)
    : dir_ (dir), current_ (logFile (dir, 0)) {
    dir_.createDirectory();   // no-op if it already exists
    // FileOutputStream opens for APPEND (it seeks to the end of any existing file),
    // so a relaunch within the same session keeps the prior trail; rotation still
    // caps the total size.
    out_ = std::make_unique<juce::FileOutputStream> (current_);
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

void DiagnosticLog::rotateIfNeeded() {
    if (out_ == nullptr || ! out_->openedOk())
        return;
    if (out_->getPosition() < kMaxBytes)
        return;

    // Close the live stream before we move files around underneath it.
    out_.reset();

    // Drop the oldest backup, then shift each remaining backup up one slot:
    // eb.(N).log -> eb.(N+1).log, oldest first so we never overwrite a file we
    // still need.
    logFile (dir_, kMaxBackups).deleteFile();
    for (int i = kMaxBackups - 1; i >= 1; --i)
        logFile (dir_, i).moveFileTo (logFile (dir_, i + 1));

    // eb.log becomes eb.1.log, then reopen a fresh, empty eb.log.
    current_.moveFileTo (logFile (dir_, 1));
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

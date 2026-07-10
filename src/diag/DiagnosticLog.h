#pragma once
#include <juce_core/juce_core.h>   // juce::File / juce::FileOutputStream / juce::String / juce::Time
#include <memory>                  // std::unique_ptr

// Per-launch, message-thread diagnostic logger (Task 1 of the match-detection +
// diagnostic-logging build). The GUI owns one instance, points it at
// %TEMP%/EarsBridge/logs, and writes lifecycle / device / Dirac / detector data
// through it across a measurement run, so a failed run leaves a readable trail.
//
// Design notes:
//   - PER-LAUNCH FILE. Each DiagnosticLog instance (i.e. each app launch) opens
//     its OWN uniquely-named file "eb-<YYYYMMDD-HHMMSS>.log" in the logs folder
//     (a millis-based uniquifier disambiguates two launches in the same second).
//     Two overlapping instances therefore NEVER share a handle, so a kill/relaunch
//     race or two instances open at once can never make the second one silently
//     fail to open the file and log nothing. If the chosen name somehow can't be
//     opened we try the next uniquifier rather than giving up silently.
//   - MESSAGE-THREAD ONLY. write()/rotateIfNeeded() do blocking file I/O (open,
//     flush, rename, delete) and are NOT real-time safe. Never call from the audio
//     callback — marshal to the message thread first. The class keeps no lock; it
//     assumes single-threaded (message-thread) use.
//   - BOUNDED FOLDER. At construction the logs folder is pruned to keep only the
//     most-recent kMaxFiles AND a total-size budget (kMaxFolderBytes), deleting
//     oldest-first, so the folder can never grow without bound across launches.
//   - SIZE-CAPPED PER FILE. THIS launch's file is capped at ~kMaxBytes; on overflow
//     it rolls to "<base>.1", "<base>.2" (deleting the oldest beyond kMaxBackups).
//     Rare with the de-duped logging, but a single runaway file is still bounded.
//   - SERIAL REDACTION. The EARS serial must NEVER appear in a log. redactSerial()
//     is defense in depth: callers also avoid logging the serial in the first
//     place, but every value that does flow through is scrubbed (in both its
//     dashed and dash-less forms) before it can land on disk.
//
// juce_core is the only dependency (a leaf utility — no GUI/audio deps).
namespace eb {

class DiagnosticLog {
public:
    // Debug is the most-detailed level. It is CAPTURED by default (written exactly like Info, just with a
    // [DBG] tag) — the point is to record MORE (every button/control state + the Start-gate reason), not to
    // gate it off. There is deliberately no runtime on/off toggle yet (YAGNI; a "Verbose" gate is reserved
    // for later). Info/Warn/Error are unchanged.
    enum class Level { Debug, Info, Warn, Error };

    // dir = the logs folder (caller passes %TEMP%/EarsBridge/logs). Creates the
    // folder if needed, prunes it to the file/size budget, then opens a fresh
    // per-launch file "eb-<YYYYMMDD-HHMMSS>.log" there for this launch.
    explicit DiagnosticLog (const juce::File& dir);

    // Append one timestamped line: "[HH:MM:SS.mmm] [LVL] " + message + "\n", then
    // flush. Rotates this launch's file first if it is already at the cap.
    // MESSAGE-THREAD ONLY.
    void write (Level level, const juce::String& message);

    juce::File directory()   const;   // the logs folder (for an "Open log folder" action)
    juce::File currentFile() const;   // THIS launch's file (eb-<stamp>.log)

    // Replace EVERY occurrence of a known serial value with "[serial redacted]",
    // matching both the value as given ("000-0000") AND its dash-less form
    // ("0000000"). A no-op when serial is empty. Pure / static — testable on its
    // own and usable by callers before they ever hand a string to write().
    [[nodiscard]] static juce::String redactSerial (const juce::String& message,
                                                    const juce::String& serial);

    static constexpr juce::int64 kMaxBytes       = 5 * 1024 * 1024;    // ~5 MB per file
    static constexpr int         kMaxBackups     = 2;                  // <base>.1, <base>.2
    static constexpr int         kMaxFiles       = 8;                  // most-recent N session logs kept
    static constexpr juce::int64 kMaxFolderBytes = 15 * 1024 * 1024;   // ~15 MB total folder budget

private:
    // Delete oldest log files (by name, which sorts by timestamp) until the folder
    // holds at most kMaxFiles AND at most kMaxFolderBytes. Called at construction,
    // BEFORE this launch's file is opened so the new file always survives.
    void pruneFolder() const;

    // When current_ has reached kMaxBytes: close the stream, shift
    // <base>.(N) -> <base>.(N+1) (deleting the oldest beyond kMaxBackups), rename
    // <base> -> <base>.1, and reopen a fresh empty <base>.
    void rotateIfNeeded();

    juce::File dir_, current_;
    std::unique_ptr<juce::FileOutputStream> out_;
};

} // namespace eb

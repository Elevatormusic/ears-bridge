#pragma once
#include <juce_core/juce_core.h>   // juce::File / juce::FileOutputStream / juce::String / juce::Time
#include <memory>                  // std::unique_ptr

// Rotating, message-thread diagnostic logger (Task 1 of the match-detection +
// diagnostic-logging build). The GUI owns one instance, points it at
// %TEMP%/EarsBridge/logs, and writes lifecycle / device / Dirac / detector data
// through it across a measurement run, so a failed run leaves a readable trail.
//
// Design notes:
//   - MESSAGE-THREAD ONLY. write()/rotateIfNeeded() do blocking file I/O (open,
//     flush, rename, delete) and are NOT real-time safe. Never call from the audio
//     callback — marshal to the message thread first. The class keeps no lock; it
//     assumes single-threaded (message-thread) use.
//   - SIZE-CAPPED + ROTATING. Each file is capped at ~kMaxBytes; on overflow the
//     log rolls eb.log -> eb.1.log -> eb.2.log and deletes the oldest beyond
//     kMaxBackups, so the logs dir never grows without bound (~15 MB worst case).
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
    // folder if needed and opens/creates eb.log there for append.
    explicit DiagnosticLog (const juce::File& dir);

    // Append one timestamped line: "[HH:MM:SS.mmm] [LVL] " + message + "\n", then
    // flush. Rotates first if the current file is already at the cap.
    // MESSAGE-THREAD ONLY.
    void write (Level level, const juce::String& message);

    juce::File directory()   const;   // the logs folder (for an "Open log folder" action)
    juce::File currentFile() const;   // eb.log

    // Replace EVERY occurrence of a known serial value with "[serial redacted]",
    // matching both the value as given ("000-0000") AND its dash-less form
    // ("0000000"). A no-op when serial is empty. Pure / static — testable on its
    // own and usable by callers before they ever hand a string to write().
    [[nodiscard]] static juce::String redactSerial (const juce::String& message,
                                                    const juce::String& serial);

    static constexpr juce::int64 kMaxBytes   = 5 * 1024 * 1024;   // ~5 MB per file
    static constexpr int         kMaxBackups = 2;                 // eb.1.log, eb.2.log -> ~15 MB total

private:
    // When current_ has reached kMaxBytes: close the stream, shift
    // eb.(N).log -> eb.(N+1).log (deleting the oldest beyond kMaxBackups), rename
    // eb.log -> eb.1.log, and reopen a fresh empty eb.log.
    void rotateIfNeeded();

    juce::File dir_, current_;
    std::unique_ptr<juce::FileOutputStream> out_;
};

} // namespace eb

#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/SystemA11y.h"

namespace eb {

// P4 motion foundation (spec §9 [P4-refresh 2026-07-05]). The app's ONE motion vocabulary: a
// one-shot 0->1 ramp, kMotionMs long, ease-out cubic, driven by its OWN self-terminating 60 Hz
// juce::Timer — NEVER the 30 Hz app tick. The repaint-on-delta discipline holds: a ramp repaints
// only its owner (via onTick), only while running, and stops itself at 1.0. Reduce Motion
// (SystemA11y) snaps to 1 with no timer. Motion never carries meaning — it only eases a state
// change the model already made, so a snapped ramp is always correct.
// Rest state: a never-started ramp reads value() == 1.0 (consumers multiply alphas by value();
// a fresh component must render at full strength).
class MotionRamp : private juce::Timer {
public:
    static constexpr int kMotionMs = 160;   // frames pin .15s-.18s; one duration app-wide

    void start() {
        stopTimer();
        if (SystemA11y::reduceMotion()) { t_ = 1.0f; fire(); return; }   // RM: snap, no timer
        t_ = 0.0f;
        startMs_ = juce::Time::getMillisecondCounter();
        fire();
        startTimerHz (60);
    }
    void snapToEnd()     { stopTimer(); if (t_ < 1.0f) { t_ = 1.0f; fire(); } }
    void finishForTest() { snapToEnd(); }   // documented seam: deterministic completion, headless
    float value() const  { const float u = 1.0f - t_; return 1.0f - u * u * u; }  // ease-out cubic
    bool  running() const { return isTimerRunning(); }
    std::function<void()> onTick;           // apply hook; the ramp has already advanced when it fires

private:
    void fire() { if (onTick) onTick(); }
    void timerCallback() override {
        const auto elapsed = (int) (juce::Time::getMillisecondCounter() - startMs_);
        t_ = juce::jlimit (0.0f, 1.0f, (float) elapsed / (float) kMotionMs);
        if (t_ >= 1.0f) stopTimer();        // self-terminating: zero standing timers at rest
        fire();
    }
    float t_ = 1.0f;                        // rest = completed
    juce::uint32 startMs_ = 0;
};

} // namespace eb

#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/Theme.h"
#include "gui/Glyphs.h"
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"
#include "gui/FormatCluster.h"
#include "audio/CombineMode.h"
#include "gui/MainComponent.h"       // P2.9 T11: evidence rig drives the real MainComponent offscreen
#include "gui/stages/CalibrateStage.h"
#include "gui/WizardState.h"         // WizardStep
#include "gui/SystemA11y.h"          // P4 T4: controlBorder's Increase-Contrast arm

// P2.9 Task 1: the two unfrozen tokens, proven through the REAL gate path (probe -> scoreDescriptor),
// not a private re-implementation of WCAG. A Label carrying accentText on the window bg must score
// clean in BOTH themes; a deliberately-faint label on the same rig must be flagged (bite proof).
namespace {
int contrastFindings (juce::Component& rig, const juce::File& tmp) {
    auto jf = tmp.getChildFile ("t.json");
    hig::writeDesignProbe (rig, jf, tmp.getChildFile ("t.png"));
    int n = 0;
    for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
        if (f.category == "contrast") ++n;
    return n;
}
}

TEST_CASE("P2.9 tokens: accentText clears 4.5:1 on bg in both themes; the rig can bite") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        struct Rig : juce::Component { juce::Label ok, bad; } rig;
        rig.setSize (300, 60);
        for (auto* l : { &rig.ok, &rig.bad }) {
            l->setColour (juce::Label::backgroundColourId, eb::Theme::bg());   // explicit bg: the exact scored pair
            l->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            rig.addAndMakeVisible (*l);
        }
        rig.ok.setText ("STEP 2 OF 4", juce::dontSendNotification);
        rig.ok.setColour (juce::Label::textColourId, eb::Theme::accentText());
        rig.ok.setBounds (0, 0, 200, 16);
        rig.bad.setText ("faint", juce::dontSendNotification);
        // Bite control: an OPAQUE near-bg tone. (NOT textFaint: the probe's hexOf DROPS alpha, so a
        // translucent-white token scores as pure #FFFFFF and can never bite on a dark bg.)
        rig.bad.setColour (juce::Label::textColourId, eb::Theme::bg().brighter (0.1f));
        rig.bad.setBounds (0, 30, 200, 16);
        INFO ((dark ? "dark" : "light"));
        // Exactly ONE contrast finding: the near-bg label bites (proves the rig is live), accentText does not.
        CHECK (contrastFindings (rig, tmp) == 1);
    }
    theme.setDarkForTest (wasDark);
    tmp.deleteRecursively();
}

TEST_CASE("P2.9 tokens: primaryFill values are the W2/M3 pins") {
    eb::Theme theme;   // registers palettes
    const bool wasDark = eb::Theme::dark();
    theme.setDarkForTest (true);
    CHECK (eb::Theme::primaryFill()  == juce::Colour (0xff0A6FE0));   // W2 --accent-fill
    CHECK (eb::Theme::accentText()   == juce::Colour (0xff0091FF));   // W2 --accent as text (5.16:1 on #1E1E1E)
    CHECK (eb::Theme::accentHover()  == juce::Colour (0xff1F9DFF));   // W2 --accent-h (already pinned)
    theme.setDarkForTest (false);
    CHECK (eb::Theme::primaryFill()  == juce::Colour (0xff0067D6));   // P4: darkened for the 4.5:1 white-label floor (was #0088FF = 3.52:1; frozen decision 4)
    CHECK (eb::Theme::accentText()   == juce::Colour (0xff0067D6));   // 4.55:1 on #ECECEE
    theme.setDarkForTest (wasDark);
}

// P4 Task 4: the token completion — M2 disabled-primary fill, M4 controlBorder, the light-CTA
// retoken — proven against eb::hig::contrastRatio (the codebase's ONE WCAG implementation, the
// same math scoreDescriptor runs), not a private re-derivation.
namespace {
juce::String hex6 (juce::Colour c) { return "#" + c.toDisplayString (false); }   // probe hexOf format
} // namespace

TEST_CASE("P4 tokens: light CTA - white label clears 4.5:1 on the darkened primaryFill (M3-light)") {
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    theme.setDarkForTest (false);
    CHECK (eb::Theme::primaryFill() == juce::Colour (0xff0067D6));   // frozen decision 4 (was #0088FF, 3.52:1)
    CHECK (eb::hig::contrastRatio (hex6 (juce::Colours::white), hex6 (eb::Theme::primaryFill())) >= 4.5);
    CHECK (eb::Theme::accentHover() == juce::Colour (0xff005CC2));   // hover darkens in light (6.35:1 w/ white)
    theme.setDarkForTest (true);
    CHECK (eb::Theme::primaryFill() == juce::Colour (0xff0A6FE0));   // dark W2 pin UNCHANGED
    CHECK (eb::hig::contrastRatio (hex6 (juce::Colours::white), hex6 (eb::Theme::primaryFill())) >= 4.5);
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P4 tokens: M4 controlBorder clears the 3:1 boundary floor vs bg in both themes") {
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    eb::SystemA11y::setForTest (false, false, false);
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        const auto composite = eb::Theme::bg().overlaidWith (eb::Theme::controlBorder());
        INFO ((dark ? "dark " : "light ") << hex6 (composite));
        CHECK (eb::hig::contrastRatio (hex6 (composite), hex6 (eb::Theme::bg())) >= 3.0);   // WCAG 1.4.11
    }
    // Increase Contrast strengthens it further (same pattern as sep()).
    theme.setDarkForTest (true);
    const float base = eb::Theme::controlBorder().getFloatAlpha();
    eb::SystemA11y::setForTest (false, true, false);
    CHECK (eb::Theme::controlBorder().getFloatAlpha() > base);
    eb::SystemA11y::setForTest (false, false, false);
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P4 tokens: M2 disabled primary keeps a recognisable blue identity, distinct from ctrl()") {
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        const auto disabled = eb::Theme::bg().overlaidWith (eb::Theme::primaryFillDisabled());
        const auto secondary = eb::Theme::ctrl();
        // The M2 essence: a disabled primary must NOT read as an inert secondary. Blue dominance +
        // a clear channel separation from the secondary fill.
        CHECK (disabled.getBlue() > disabled.getRed() + 30);
        const int maxDelta = juce::jmax (std::abs ((int) disabled.getRed()   - (int) secondary.getRed()),
                                         std::abs ((int) disabled.getGreen() - (int) secondary.getGreen()),
                                         std::abs ((int) disabled.getBlue()  - (int) secondary.getBlue()));
        INFO ((dark ? "dark" : "light"));
        CHECK (maxDelta > 30);
        CHECK (disabled != eb::Theme::bg().overlaidWith (eb::Theme::primaryFill()));   // dimmed, not enabled-look
    }
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P4 tokens: buttonLabelColour - enabled primary is white both themes; disabled keeps text()") {
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    juce::TextButton primary ("Start monitoring"), secondary ("Stop");
    primary.getProperties().set ("primary", true);
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        primary.setEnabled (true);
        CHECK (eb::Theme::buttonLabelColour (primary) == eb::Theme::onAccentText());
        primary.setEnabled (false);
        CHECK (eb::Theme::buttonLabelColour (primary) == primary.findColour (juce::TextButton::textColourOffId));
        CHECK (eb::Theme::buttonLabelColour (secondary) == secondary.findColour (juce::TextButton::textColourOffId));
    }
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P4 tokens: painted truth - disabled/enabled primary capsule fills sample to the tokens") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        struct Rig : juce::Component { juce::TextButton b { "Start" };
            Rig() { addAndMakeVisible (b); b.getProperties().set ("primary", true); }
            void resized() override { b.setBounds (getLocalBounds().reduced (10)); }
            void paint (juce::Graphics& g) override { g.fillAll (eb::Theme::bg()); } } rig;
        rig.setLookAndFeel (&theme);   // brief defect fix: a bare rig paints via the stock V4 LnF, not Theme
        rig.setSize (160, 54);
        rig.resized();
        auto sample = [&] { const auto img = rig.createComponentSnapshot (rig.getLocalBounds(), true, 1.0f);
                            return img.getPixelAt (30, 27); };
        // ^ mid-height, left of the label run = pure fill on every platform font. (The brief's (80,27)
        //   sat mid-"Start" and only passed via the glyph counter - macOS CI metrics differ.)
        rig.b.setEnabled (false);
        const auto expDis = eb::Theme::bg().overlaidWith (eb::Theme::primaryFillDisabled());
        const auto gotDis = sample();
        INFO ((dark ? "dark" : "light") << " disabled got " << gotDis.toDisplayString (true));
        CHECK (std::abs ((int) gotDis.getRed()   - (int) expDis.getRed())   <= 2);
        CHECK (std::abs ((int) gotDis.getGreen() - (int) expDis.getGreen()) <= 2);
        CHECK (std::abs ((int) gotDis.getBlue()  - (int) expDis.getBlue())  <= 2);
        rig.b.setEnabled (true);
        CHECK (sample() == eb::Theme::primaryFill());                 // enabled idle = the token, exact
    }
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P2.9 glyphs: every glyph renders inside its box and never bleeds outside") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    using Fn = void (*) (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    struct G { const char* name; Fn fn; };
    const G glyphs[] = {
        { "play",    eb::glyph::drawPlay },    { "stop",   eb::glyph::drawStop },
        { "tick",    eb::glyph::drawTick },    { "info",   eb::glyph::drawInfo },
        { "refresh", eb::glyph::drawRefresh }, { "folder", eb::glyph::drawFolder },
        { "export",  eb::glyph::drawExport },  { "warning", eb::glyph::drawWarning },
    };
    for (const auto& g : glyphs) {
        juce::Image img (juce::Image::ARGB, 24, 24, true);
        { juce::Graphics gc (img); g.fn (gc, { 4.0f, 4.0f, 16.0f, 16.0f }, juce::Colours::white); }
        int inside = 0, outside = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 24; ++x) {
                if (img.getPixelAt (x, y).getAlpha() == 0) continue;
                const bool in = x >= 3 && x < 21 && y >= 3 && y < 21;   // box + 1px stroke tolerance
                (in ? inside : outside)++;
            }
        INFO (g.name << " inside=" << inside << " outside=" << outside);
        CHECK (inside > 12);      // it drew something substantive
        CHECK (outside == 0);     // and stayed in its box
    }
}

TEST_CASE("P2.9 format cluster: parts render W2's wording from live setting values") {
    auto p = eb::formatClusterParts (48000.0, 24, eb::CombineMode::AutoPerEar);
    CHECK (p[0] == "48 kHz"); CHECK (p[1] == "24-bit"); CHECK (p[2] == "Auto per-ear");
    p = eb::formatClusterParts (44100.0, 16, eb::CombineMode::Sum);
    CHECK (p[0] == "44.1 kHz"); CHECK (p[1] == "16-bit"); CHECK (p[2] == "Sum");
    p = eb::formatClusterParts (96000.0, 32, eb::CombineMode::LeftOnly);
    CHECK (p[0] == "96 kHz"); CHECK (p[2] == "Left ear");
}

// P2.9 Task 11 (Rule-2 evidence): offscreen snapshots of every changed surface in BOTH themes.
// HIDDEN — tag [.evidence] never runs in CI; run explicitly: eb_tests "[.evidence]".
// createComponentSnapshot is a peer-free top-down paint, so this needs no window/DPI and is hermetic
// (temp-dir Settings, network suppressed, placeholder 000-0000 fixtures only). 12 PNGs out.
TEST_CASE("P2.9 evidence: offscreen captures of every changed surface, both themes", "[.evidence]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile ("build/design-evidence");
    out.createDirectory();
    const juce::File data (EB_TEST_DATA_DIR);
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    const auto shoot = [&] (const juce::String& name) {
        auto img = mc.createComponentSnapshot (mc.getLocalBounds());
        juce::PNGImageFormat png;
        juce::FileOutputStream os (out.getChildFile (name + ".png"));
        REQUIRE (os.openedOk());
        os.setPosition (0);          // FileOutputStream appends: truncate, or a re-run into the persistent
        os.truncate();               // evidence dir keeps decoding the first-ever capture
        png.writeImageToStream (img, os);
    };
    for (bool dark : { true, false }) {
        mc.forceThemeForTest (dark);
        const juce::String t = dark ? "dark-" : "light-";
        mc.forceWizardStepForTest (eb::WizardStep::Connect);   shoot (t + "connect");
        mc.driveConnectWarningsForTest (true, true);
        mc.forceWizardStepForTest (eb::WizardStep::Connect);   shoot (t + "connect-warned");
        mc.driveConnectWarningsForTest (false, false);
        REQUIRE (mc.leftCalForTest().loadFromFile (data.getChildFile ("L_HEQ_0000000.txt")));
        REQUIRE (mc.rightCalForTest().loadFromFile (data.getChildFile ("R_HEQ_0000000.txt")));
        mc.forceWizardStepForTest (eb::WizardStep::Calibrate); shoot (t + "calibrate-loaded");
        mc.calibrateStageForTest().setAdvancedOpen (true);
        mc.forceWizardStepForTest (eb::WizardStep::Calibrate); shoot (t + "calibrate-advanced-trimrow");
        mc.calibrateStageForTest().setAdvancedOpen (false);
        mc.leftCalForTest().clearCal(); mc.rightCalForTest().clearCal();
        mc.forceWizardStepForTest (eb::WizardStep::Level);     shoot (t + "level");
        mc.forceWizardStepForTest (eb::WizardStep::Measure);   shoot (t + "measure");
    }
    tmp.deleteRecursively();
}

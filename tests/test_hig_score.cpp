#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"

// Task 1: the probe's `role` field was declared + emitted but never assigned (always ""). It is now derived
// from the component type (and overridden by the accessibility handler when a peer is attached). This test
// also doubles as the first proof that describeComponentTree works on a stock widget HEADLESS (no peer).
TEST_CASE("probe: a TextButton emits a non-empty role and a valid descriptor") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::TextButton b ("Go");
    b.setSize (80, 30);
    const auto json = hig::describeComponentTree (b);
    auto v = juce::JSON::parse (json);
    REQUIRE (v.isObject());
    auto* elements = v.getProperty ("elements", {}).getArray();
    REQUIRE (elements != nullptr);
    REQUIRE (elements->size() >= 1);
    const auto& root = elements->getReference (0);
    CHECK (root.getProperty ("type", {}).toString() == "TextButton");
    CHECK (root.getProperty ("role", {}).toString().isNotEmpty());   // was always "" before the fix
}

// ---- HigScore descriptor fixtures (Tasks 3-4) ----
static juce::var makeDescriptor (juce::Array<juce::var> els) {
    auto* top = new juce::DynamicObject();
    auto* meta = new juce::DynamicObject();
    top->setProperty ("meta", juce::var (meta));
    top->setProperty ("elements", els);
    return juce::var (top);
}
static juce::var el (const juce::String& type, const juce::String& label,
                     int x, int y, int w, int h,
                     const juce::String& fg = "#000000", const juce::String& bg = "#ffffff",
                     bool measurable = true, bool fgOk = true, bool bgOk = true,
                     double fontPt = 13.0, bool bold = false, bool textOverflows = false,
                     const juce::String& role = "text") {
    auto* o = new juce::DynamicObject();
    auto* b = new juce::DynamicObject();
    b->setProperty("x",x); b->setProperty("y",y); b->setProperty("w",w); b->setProperty("h",h);
    o->setProperty("id", type+label); o->setProperty("type", type); o->setProperty("role", role);
    o->setProperty("label", label); o->setProperty("value", juce::String());
    o->setProperty("bounds", juce::var(b)); o->setProperty("fg", fg); o->setProperty("bg", bg);
    o->setProperty("fgIntrospectable", fgOk); o->setProperty("bgIntrospectable", bgOk);
    o->setProperty("fontPt", fontPt); o->setProperty("bold", bold);
    o->setProperty("visible", true); o->setProperty("showing", true); o->setProperty("enabled", true);
    o->setProperty("measurable", measurable); o->setProperty("textOverflows", textOverflows);
    return juce::var(o);
}

TEST_CASE("HigScore: low-contrast measurable label is a contrast finding") {
    // grey #777777 on white ~4.48:1 < 4.5 normal floor -> medium contrast finding
    auto d = makeDescriptor ({ el ("Label", "hi", 0, 0, 50, 18, "#777777", "#ffffff") });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1);
    CHECK (f[0].category == "contrast");
    CHECK (f[0].severity == "medium");
}
TEST_CASE("HigScore: custom/non-introspectable nodes are never contrast-scored") {
    auto d = makeDescriptor ({ el ("custom/unknown", "x", 0, 0, 50, 18, "not introspectable",
                                   "not introspectable", /*measurable*/false, /*fgOk*/false, /*bgOk*/false) });
    CHECK (eb::hig::scoreDescriptor (d).empty());
}
TEST_CASE("HigScore: sufficient-contrast black-on-white label is clean") {
    auto d = makeDescriptor ({ el ("Label", "hi", 0, 0, 50, 18, "#000000", "#ffffff") });
    CHECK (eb::hig::scoreDescriptor (d).empty());
}

// ---- geometry (Task 4): overlap / duplicate / target-size / clip ----
TEST_CASE("HigScore: two overlapping non-identical siblings -> one overlap finding") {
    auto d = makeDescriptor ({ el ("Label","a",0,0,100,20), el ("Label","b",90,0,100,20) }); // 10px deep
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "overlap");
}
TEST_CASE("HigScore: overlap of exactly 2px does NOT report (>2 strict)") {
    auto d = makeDescriptor ({ el ("Label","a",0,0,100,20), el ("Label","b",98,0,100,20) }); // 2px deep
    CHECK (eb::hig::scoreDescriptor (d).empty());
}
TEST_CASE("HigScore: a child fully inside its parent is nested, not an overlap") {
    auto d = makeDescriptor ({ el ("Label","parent",0,0,100,100), el ("Label","child",10,10,50,50) });
    CHECK (eb::hig::scoreDescriptor (d).empty());
}
TEST_CASE("HigScore: identical stacked -> duplicate medium; identical overlapping -> duplicate high") {
    auto stacked = makeDescriptor ({ el ("Label","Same",0,0,40,20), el ("Label","Same",0,40,40,20) });
    auto fs = eb::hig::scoreDescriptor (stacked);
    REQUIRE (fs.size() == 1); CHECK (fs[0].category == "duplicate"); CHECK (fs[0].severity == "medium");
    auto over = makeDescriptor ({ el ("Label","Same",0,0,40,20), el ("Label","Same",10,0,40,20) });
    auto fo = eb::hig::scoreDescriptor (over);
    REQUIRE (fo.size() == 1); CHECK (fo[0].category == "duplicate"); CHECK (fo[0].severity == "high");
}
TEST_CASE("HigScore: sub-24px interactive target -> target-size") {
    auto d = makeDescriptor ({ el ("TextButton","x",0,0,20,20,"#000000","#ffffff",true,true,true,13,false,false,"button") });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "target-size");
}
TEST_CASE("HigScore: a clipped label -> clip") {
    auto d = makeDescriptor ({ el ("Label","x",0,0,40,18,"#000000","#ffffff",true,true,true,13,false,true) });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "clip");
}

#pragma once
namespace eb {
// NOTE: values are persisted (Settings combineMode). Keep them stable -- only ADD new modes at the
// END, never reorder (the GUI restores by SEARCHING combineModeOrder for the value, so the menu
// order is free). LeftOnly/RightOnly are the single-ear modes ("Left/Right ear only" in the UI).
enum class CombineMode { LeftOnly, RightOnly, Average, Sum, AutoPerEar };
}

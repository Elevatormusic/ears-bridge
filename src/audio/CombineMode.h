#pragma once
namespace eb {
// NOTE: values are persisted (Settings combineMode) and indexed by the GUI combo order, so only
// ADD new modes at the END -- never reorder.
enum class CombineMode { TwoPassLeft, TwoPassRight, Average, Sum, AutoPerEar };
}

// Private include of the vendored Bosch SensorAPI (third_party/bosch, C, unmodified). The vendor
// headers are not written against the Quartz C++ warning set, so the diagnostics are relaxed for
// the include only; our own code in this component is still compiled with the full set.
#pragma once

#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-align"
extern "C" { // bma4.h has no C++ guard
#include "bma4.h"
#include "bma423.h"
#include "bma4_defs.h"
}
#pragma GCC diagnostic pop

/// The feature-config blob: defined (non-static, 6144 bytes) in third_party/bosch/bma423.c but not
/// declared by any vendor header.
extern "C" const std::uint8_t bma423_config_file[]; // NOLINT(readability-identifier-naming)

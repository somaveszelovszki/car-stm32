#pragma once

#include <micro/utils/units.hpp>

namespace cfg {

constexpr uint8_t MAX_NUM_LINES = 3;            // Maximum number of detected lines at the same time on the track.

constexpr uint8_t MAX_NUM_LAB_SEGMENTS = 20;    // Maximum number of segments in the labyrinth.

constexpr micro::meter_t MIN_JUNCTION_LENGTH = micro::centimeter_t(20);

} // namespace cfg

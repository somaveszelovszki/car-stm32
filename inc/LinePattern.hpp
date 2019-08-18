#pragma once

#include <micro/utils/units.hpp>
#include <micro/utils/point2.hpp>

namespace micro {

/* @brief Stores data of a detected line pattern.
 **/
struct LinePattern {
public:
    static const point2<meter_t> UNKNOWN_POS;

    /* @brief Defines line pattern type.
     **/
    enum class Type : uint32_t {
        SINGLE_LINE = 0,    // 1 solid 1.9mm wide line.
        ACCELERATE  = 1,    // 1 solid line in the middle, 2 dashed lines on both sides
        BRAKE       = 2,    // 1 solid line in the middle, 2 solid lines on both sides
        LANE_CHANGE = 3,    // 1 solid line in the middle, 1 dashed line on one (UNKNOWN!) side (dash and space lengths decreasing)
        JUNCTION    = 4     // Labyrinth junction
    };

    Type type;          // Line pattern type.
    Sign dir;           // The pattern direction (POSITIVE means it is the same as the car direction). e.g. at a junction: POSITIVE means the car has two options to choose from (forward or exit)
    Direction side;   // The side of the line the pattern is on. In case of symmetrical pattern: CENTER.
    point2<meter_t> startPos;   // The pattern start position.
    point2<meter_t> endPos;     // The pattern end position.

    LinePattern()
        : type(Type::SINGLE_LINE)
        , dir(Sign::POSITIVE)
        , side(Direction::CENTER)
        , startPos(UNKNOWN_POS)
        , endPos(UNKNOWN_POS) {}
};

} // namespace micro
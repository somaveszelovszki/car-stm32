#pragma once

#include <uns/bsp/gpio.hpp>
#include <uns/util/units.hpp>

namespace uns {
namespace hw {

class HC_SR04_Ultrasonic {

public:
    HC_SR04_Ultrasonic(const gpio_pin_struct& _trigger, const gpio_pin_struct& _echo);

    void initialize();

    void startMeasurement();

    void onEchoReceived();

    distance_t getDistance() const {
        return this->distance;
    }

private:
    const gpio_pin_struct& trigger;
    const gpio_pin_struct& echo;

    time_t lastStartTime;
    bool busy;
    distance_t distance;
};

} // namespace hw
} // namespace uns

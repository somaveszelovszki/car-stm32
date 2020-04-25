#include <micro/hw/MPU9250_Gyroscope.hpp>
#include <micro/sensor/Filter.hpp>
#include <micro/utils/log.hpp>
#include <micro/utils/timer.hpp>

#include <cfg_board.h>
#include <globals.hpp>

#include <stm32f4xx_hal.h>
#include <stm32f4xx_hal_gpio.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include <utility>

using namespace micro;

namespace {

hw::MPU9250 gyro(i2c_X, hw::Ascale::AFS_2G, hw::Gscale::GFS_250DPS, hw::Mscale::MFS_16BITS, MMODE_ODR_100Hz);

class AngleCalc {
public:
    struct MagNormalization {
        gauss_t min_;
        gauss_t max_;

        void update(const gauss_t value) {
            if (this->min_ == gauss_t(0) || value < this->min_) {
                this->min_ = value;
            }
            if (this->max_ == gauss_t(0) || value > this->max_) {
                this->max_ = value;
            }
        }

        float apply(gauss_t mag) const {
            return map(mag, this->min_, this->max_, -1.0f, 1.0f);
        }
    };

    explicit AngleCalc(const point2<MagNormalization>& defaultNorm = { {}, {} }) : norm(defaultNorm) {}

    radian_t getAngle(const point3<gauss_t>& mag, bool updateNorm) {

        if (updateNorm) {
            norm.X.update(mag.X);
            norm.Y.update(mag.Y);
        }

        return micro::atan2(norm.Y.apply(mag.Y), norm.X.apply(mag.X));
    }

    bool isCalibrated() const {
        return abs(this->norm.X.max_ - this->norm.X.min_) > gauss_t(200) &&
               abs(this->norm.Y.max_ - this->norm.Y.min_) > gauss_t(200);
    }

private:
    point2<MagNormalization> norm;
    point2<std::pair<gauss_t, gauss_t>> boundaries;
};

AngleCalc DEFAULT_ANGLE_CALC({
    { gauss_t(-460), gauss_t(-26) },
    { gauss_t(295),  gauss_t(693) }
});

AngleCalc angleCalc;

void updateOrientedDistance() {
    static meter_t orientedSectionStartDist;
    static radian_t orientation;

    const bool isOriented = eqWithOverflow360(globals::car.pose.angle, orientation, degree_t(4));
    if (!isOriented) {
        orientedSectionStartDist = globals::car.distance;
        orientation = globals::car.pose.angle;
    }

    globals::car.orientedDistance = globals::car.distance - orientedSectionStartDist;
}

} // namespace

extern "C" void runGyroTask(void) {

    vTaskDelay(300); // gives time to other tasks to wake up

    gyro.initialize();

    globals::isGyroTaskOk = true;

    millisecond_t prevReadTime = getTime();
    millisecond_t lastNonZeroAngVelTime = getTime();

    while (true) {
        const point3<rad_per_sec_t> gyroData = gyro.readGyroData();
        if (gyroData.Z != micro::numeric_limits<rad_per_sec_t>::infinity()) {
            prevReadTime = getTime();

            if (gyroData.Z != rad_per_sec_t(0)) {
                lastNonZeroAngVelTime = getTime();
            }

            globals::car.yawRate = gyroData.Z;
            updateOrientedDistance();

            globals::isGyroTaskOk = getTime() - lastNonZeroAngVelTime < millisecond_t(100);

        } else if (getTime() - prevReadTime > millisecond_t(400)) {
            globals::isGyroTaskOk = false;
            gyro.initialize();
            prevReadTime = getExactTime();
        }

        vTaskDelay(1);
    }

    vTaskDelete(nullptr);
}

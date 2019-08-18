#include <micro/task/common.hpp>
#include <micro/utils/debug.hpp>
#include <micro/utils/updatable.hpp>
#include <micro/bsp/task.hpp>
#include <micro/hw/SteeringServo.hpp>
#include <micro/sensor/Filter.hpp>
#include <micro/control/LineController.hpp>
#include <micro/panel/LineDetectPanel.hpp>
#include <micro/panel/MotorPanel.hpp>

#include <cfg_board.hpp>
#include <cfg_os.hpp>
#include <cfg_car.hpp>

#include <globals.hpp>

using namespace micro;

LineDetectPanel frontLineDetectPanel(cfg::uart_FrontLineDetectPanel);
LineDetectPanel rearLineDetectPanel(cfg::uart_RearLineDetectPanel);
MotorPanel motorPanel(cfg::uart_MotorPanel);

namespace {

volatile atomic_updatable<LinePositions> frontLinePositions(cfg::mutex_FrontLinePos);
volatile atomic_updatable<LinePositions> rearLinePositions(cfg::mutex_RearLinePos);

bool isFastSpeedSafe(const Line& line) {
    static constexpr millimeter_t MAX_LINE_POS = centimeter_t(8.5f);
    static constexpr radian_t MAX_LINE_ANGLE = degree_t(8.0f);

    return abs(line.pos_front) <= MAX_LINE_POS && abs(line.angle) <= MAX_LINE_ANGLE;
}

} // namespace

extern "C" void runControlTask(const void *argument) {

    Lines lines;
    Line mainLine;
    LineController lineController(cfg::WHEEL_BASE, cfg::WHEEL_LED_DIST);
    hw::SteeringServo steeringServo(cfg::tim_SteeringServo, cfg::tim_chnl_SteeringServo1, cfg::SERVO_MID, cfg::WHEEL_MAX_DELTA, cfg::SERVO_WHEEL_TR);

    while(!task::hasErrorHappened()) {
        CarProps car;
        globals::car.wait_copy(car);

        if (frontLinePositions.is_updated() && rearLinePositions.is_updated()) {

            LinePositions front, rear;
            frontLinePositions.wait_copy(front);
            rearLinePositions.wait_copy(rear);
            calculateLines(front, rear, lines, mainLine);

            if (globals::lineFollowEnabled) {
                const meter_t baseline = meter_t::ZERO();   // TODO change baseline for more efficient turns
                if (isOk(lineController.run(car.speed, baseline, mainLine))) {
                    steeringServo.writeWheelAngle(lineController.getOutput());
                }
            }
        }

        nonBlockingDelay(millisecond_t(1));
    }

    taskDeleteCurrent();
}

/* @brief Callback for motor panel UART RxCplt - called when receive finishes.
 */
void micro_MotorPanel_Uart_RxCpltCallback() {
    // TODO
}

/* @brief Callback for front line detect panel UART RxCplt - called when receive finishes.
 */
void micro_FrontLineDetectPanel_Uart_RxCpltCallback() {
    LinePositions *p = const_cast<LinePositions*>(frontLinePositions.accept_ptr());
    if (p) {
        frontLineDetectPanel.getLinePositions(*p);
        frontLinePositions.release_ptr();
    }
}

/* @brief Callback for rear line detect panel UART RxCplt - called when receive finishes.
 */
void micro_RearLineDetectPanel_Uart_RxCpltCallback() {
    LinePositions *p = const_cast<LinePositions*>(rearLinePositions.accept_ptr());
    if (p) {
        rearLineDetectPanel.getLinePositions(*p);
        rearLinePositions.release_ptr();
    }
}



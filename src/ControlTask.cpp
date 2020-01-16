#include <micro/utils/Line.hpp>
#include <micro/utils/log.hpp>
#include <micro/utils/timer.hpp>
#include <micro/utils/updatable.hpp>
#include <micro/hw/SteeringServo.hpp>
#include <micro/sensor/Filter.hpp>
#include <micro/control/PD_Controller.hpp>
#include <micro/panel/MotorPanelLink.hpp>
#include <micro/task/common.hpp>

#include <cfg_board.h>
#include <cfg_car.hpp>
#include <globals.hpp>
#include <ControlData.hpp>

#include <FreeRTOS.h>
#include <queue.h>

using namespace micro;

#define CONTROL_QUEUE_LENGTH 1
QueueHandle_t controlQueue;
static uint8_t controlQueueStorageBuffer[CONTROL_QUEUE_LENGTH * sizeof(ControlData)];
static StaticQueue_t controlQueueBuffer;

namespace {

hw::SteeringServo frontSteeringServo(
    tim_SteeringServo, tim_chnl_FrontServo,
    cfg::FRONT_SERVO_PWM_0, cfg::FRONT_SERVO_PWM_180,
    cfg::FRONT_SERVO_OFFSET, cfg::FRONT_SERVO_WHEEL_MAX_DELTA,
    cfg::FRONT_SERVO_WHEEL_TR);

hw::SteeringServo rearSteeringServo(
    tim_SteeringServo, tim_chnl_RearServo,
    cfg::REAR_SERVO_PWM_0, cfg::REAR_SERVO_PWM_180,
    cfg::REAR_SERVO_OFFSET, cfg::REAR_SERVO_WHEEL_MAX_DELTA,
    cfg::REAR_SERVO_WHEEL_TR);

hw::Servo frontDistServo(
    tim_ServoX, tim_chnl_ServoX1,
    cfg::DIST_SERVO_PWM_0, cfg::DIST_SERVO_PWM_180,
    cfg::DIST_SERVO_OFFSET, cfg::DIST_SERVO_MAX_DELTA);

static Timer frontDistServoUpdateTimer;

void fillMotorPanelData(motorPanelDataIn_t& txData, m_per_sec_t targetSpeed) {
    txData.controller_P            = globals::motorCtrl_P;
    txData.controller_I            = globals::motorCtrl_I;
    txData.controller_integral_max = globals::motorCtrl_integral_max;
    txData.targetSpeed_mmps        = static_cast<int16_t>(static_cast<mm_per_sec_t>(targetSpeed).get());

    txData.flags = 0x00;
    if (globals::useSafetyEnableSignal) txData.flags |= MOTOR_PANEL_FLAG_USE_SAFETY_SIGNAL;
}

static void parseMotorPanelData(motorPanelDataOut_t& rxData) {
    globals::car.distance = millimeter_t(rxData.distance_mm);
    globals::car.speed = mm_per_sec_t(rxData.actualSpeed_mmps);
}

volatile bool newData = false;

void startPanel() {
    millisecond_t prevSendTime = millisecond_t(0);
    char startChar = 'S';
    do {
        if (getTime() - prevSendTime > millisecond_t(50)) {
            HAL_UART_Transmit_DMA(uart_MotorPanel, (uint8_t*)&startChar, 1);
            prevSendTime = getTime();
        }
        vTaskDelay(5);
    } while (!newData);
    newData = false;
}

} // namespace

extern "C" void runControlTask(const void *argument) {
    controlQueue = xQueueCreateStatic(CONTROL_QUEUE_LENGTH, sizeof(ControlData), controlQueueStorageBuffer, &controlQueueBuffer);

    motorPanelDataOut_t rxData;
    motorPanelDataIn_t txData;
    HAL_UART_Receive_DMA(uart_MotorPanel, (uint8_t*)&rxData, sizeof(motorPanelDataOut_t));

    vTaskDelay(10); // gives time to other tasks to wake up

    frontSteeringServo.writeWheelAngle(radian_t::zero());
    rearSteeringServo.writeWheelAngle(radian_t::zero());
    frontDistServo.write(radian_t::zero());

    ControlData controlData;
    millisecond_t lastControlDataRecvTime = millisecond_t::zero();

    PD_Controller lineController(globals::frontLineCtrl_P_slow, globals::frontLineCtrl_D_slow,
        static_cast<degree_t>(-cfg::FRONT_SERVO_WHEEL_MAX_DELTA).get(), static_cast<degree_t>(cfg::FRONT_SERVO_WHEEL_MAX_DELTA).get());

    startPanel();

    millisecond_t prevSendTime = millisecond_t(0);
    globals::isControlTaskOk = true;
    frontDistServoUpdateTimer.start(millisecond_t(20));

    while (true) {
        if (newData) {
            newData = false;
            parseMotorPanelData(rxData);
        }

        // if no control data is received for a given period, stops motor for safety reasons
        if (xQueueReceive(controlQueue, &controlData, 0)) {
            lastControlDataRecvTime = getTime();

            lineController.setParams(globals::frontLineCtrl_P_slow, globals::frontLineCtrl_D_slow);

            lineController.run(static_cast<centimeter_t>(controlData.baseline.pos - controlData.offset).get());

            frontSteeringServo.writeWheelAngle(controlData.angle + degree_t(lineController.getOutput()));
            rearSteeringServo.writeWheelAngle(controlData.angle - degree_t(lineController.getOutput()));

        } else if (getTime() - lastControlDataRecvTime > millisecond_t(20)) {
            controlData.speed = m_per_sec_t::zero();
        }

        if (globals::distServoEnabled && frontDistServoUpdateTimer.checkTimeout()) {
            frontDistServo.write(frontSteeringServo.wheelAngle() * globals::distServoTransferRate);
        }

        if (getTime() - prevSendTime > millisecond_t(20)) {
            fillMotorPanelData(txData, controlData.speed);
            HAL_UART_Transmit_DMA(uart_MotorPanel, (uint8_t*)&txData, sizeof(motorPanelDataIn_t));
            prevSendTime = getTime();
        }

        vTaskDelay(1);
    }

    vTaskDelete(nullptr);
}

/* @brief Callback for motor panel UART RxCplt - called when receive finishes.
 */
void micro_MotorPanel_Uart_RxCpltCallback(const uint32_t leftBytes) {
    newData = true;
}

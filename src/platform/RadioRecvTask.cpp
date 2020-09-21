#include <micro/debug/SystemManager.hpp>
#include <micro/port/gpio.hpp>
#include <micro/port/task.hpp>
#include <micro/utils/log.hpp>
#include <micro/utils/timer.hpp>

#include <cfg_board.hpp>
#include <cfg_track.hpp>

using namespace micro;

queue_t<uint8_t, 1> radioRecvQueue;

namespace {
volatile millisecond_t lastRxTime;
} // namespace

extern "C" void runRadioRecvTask(void) {

    SystemManager::instance().registerTask();

    millisecond_t lastQueueSendTime;
    uint8_t radioRecvValue = 0;
    uart_receive(uart_RadioModule, &radioRecvValue, 1);

    while (true) {
        if (lastRxTime != lastQueueSendTime) {
            radioRecvQueue.overwrite(radioRecvValue);
            lastQueueSendTime = lastRxTime;
        }
        os_sleep(millisecond_t(20));
    }
}

/* @brief Callback for RadioModule UART RxCplt - called when receive finishes.
 */
void micro_RadioModule_Uart_RxCpltCallback() {
    lastRxTime = getTime();
}

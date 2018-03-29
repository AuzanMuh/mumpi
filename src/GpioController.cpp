#include "GpioController.hpp"

GpioController::GpioController() {
}
GpioController::~GpioController() {
}

void GpioController::setup() {
}

void GpioController::checkSwitchesChannelId() {
    GPIO::DigitalIn chAll(PIN_ALL_CHANNEL);
    GPIO::DigitalIn ch1(PIN_CHANNEL_1);
    GPIO::DigitalIn ch2(PIN_CHANNEL_2);
    GPIO::DigitalIn ch3(PIN_CHANNEL_3);
    
    if (chAll.get_state()) {
        channel_id_state = 1000;
    } else if(ch1.get_state()) {
        channel_id_state = 0;
    } else if (ch2.get_state()) {
        channel_id_state = 1;
    } else if (ch3.get_state()) {
        channel_id_state = 2;
    }
}

bool GpioController::checkButtonPushToTalk() {
    GPIO::PushButton push(PIN_PTT, GPIO::GPIO_PULL::UP);
    return push.is_on();
}

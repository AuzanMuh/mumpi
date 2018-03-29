#ifndef GpioController_hpp
#define GpioController_hpp

#include <cppgpio.hpp>
#include <log4cpp/Category.hh>

const int PIN_ALL_CHANNEL = 19;
const int PIN_CHANNEL_1 = 20;
const int PIN_CHANNEL_2 = 21;
const int PIN_CHANNEL_3 = 23;
const int PIN_VOX = 24;
const int PIN_PTT = 25;

/**
 * GPIO class controller to read and write from gpio pin.
 */
class GpioController{
public:
    GpioController();
    ~GpioController();

    void setup();
    void checkSwitchesChannelId();
    bool checkButtonPushToTalk();
    int getChannelId() const {return channel_id_state;};
private:
    int channel_id_state = 0;
    log4cpp::Category& _logger = log4cpp::Category::getInstance("mumpi.GpioController");
};

#endif /* GpioController */
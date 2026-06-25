#ifndef SERVO_DRIVER_H
#define SERVO_DRIVER_H

#include <Arduino.h>
#include "driver/ledc.h"

class Servo180Driver {
public:
    Servo180Driver(int min_pulse_us = 500, int max_pulse_us = 2500,
                   int min_angle_deg = 60, int max_angle_deg = 80);

    bool attach(int pin);
    void detach();
    bool attached() const;
    int pin() const;

    void set_angle(int angle_deg);
    int current_angle() const;

private:
    int pin_ = -1;
    bool is_attached_ = false;
    int min_pulse_us_;
    int max_pulse_us_;
    int min_angle_deg_;
    int max_angle_deg_;
    int current_angle_deg_ = 90;
    ledc_timer_t ledc_timer_;
    ledc_channel_t ledc_channel_;
};

#endif

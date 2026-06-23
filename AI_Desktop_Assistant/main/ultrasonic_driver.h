#ifndef ULTRASONIC_DRIVER_H
#define ULTRASONIC_DRIVER_H

#include <Arduino.h>

bool ultrasonic_init(int trig_pin, int echo_pin);
bool ultrasonic_is_ready();
bool ultrasonic_read_cm(float& distance_cm, uint32_t timeout_us = 30000);
float ultrasonic_last_distance_cm();

#endif

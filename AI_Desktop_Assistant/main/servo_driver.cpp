#include "servo_driver.h"

#define SERVO_PWM_FREQ     50
#define SERVO_PERIOD_US    20000U
#define SERVO_DUTY_MAX     ((1U << 14) - 1)

static ledc_timer_t s_next_timer = LEDC_TIMER_1;
static ledc_channel_t s_next_channel = LEDC_CHANNEL_2;

static uint32_t pulse_to_duty(int pulse_us) {
    if (pulse_us <= 0) return 0;
    return ((uint32_t)pulse_us * SERVO_DUTY_MAX) / SERVO_PERIOD_US;
}

// ===== Servo180Driver =====

Servo180Driver::Servo180Driver(int min_pulse_us, int max_pulse_us)
    : min_pulse_us_(min_pulse_us), max_pulse_us_(max_pulse_us),
      ledc_timer_(s_next_timer), ledc_channel_(s_next_channel) {
    s_next_channel = (ledc_channel_t)((int)s_next_channel + 1);
    if ((int)s_next_channel % 2 == 0) {
        s_next_timer = (ledc_timer_t)((int)s_next_timer + 1);
    }
}

bool Servo180Driver::attach(int pin) {
    if (is_attached_) return true;

    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.duty_resolution = LEDC_TIMER_14_BIT;
    timer_conf.timer_num = ledc_timer_;
    timer_conf.freq_hz = SERVO_PWM_FREQ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer_conf) != ESP_OK) return false;

    ledc_channel_config_t chan_conf = {};
    chan_conf.gpio_num = pin;
    chan_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    chan_conf.channel = ledc_channel_;
    chan_conf.intr_type = LEDC_INTR_DISABLE;
    chan_conf.timer_sel = ledc_timer_;
    chan_conf.duty = 0;
    chan_conf.hpoint = 0;
    if (ledc_channel_config(&chan_conf) != ESP_OK) return false;

    pin_ = pin;
    is_attached_ = true;
    return true;
}

void Servo180Driver::detach() {
    if (!is_attached_) return;
    ledc_stop(LEDC_LOW_SPEED_MODE, ledc_channel_, 0);
    is_attached_ = false;
    pin_ = -1;
}

bool Servo180Driver::attached() const { return is_attached_; }
int Servo180Driver::pin() const { return pin_; }

void Servo180Driver::set_angle(int angle_deg) {
    if (!is_attached_) return;
    if (angle_deg < 0) angle_deg = 0;
    if (angle_deg > 180) angle_deg = 180;
    current_angle_deg_ = angle_deg;

    int pulse_us = map(angle_deg, 0, 180, min_pulse_us_, max_pulse_us_);
    uint32_t duty = pulse_to_duty(pulse_us);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
}

int Servo180Driver::current_angle() const { return current_angle_deg_; }

// ===== Servo360Driver =====

Servo360Driver::Servo360Driver(int min_pulse_us, int max_pulse_us, int stop_pulse_us)
    : min_pulse_us_(min_pulse_us),
      max_pulse_us_(max_pulse_us),
      stop_pulse_us_(stop_pulse_us),
      ledc_timer_(s_next_timer), ledc_channel_(s_next_channel) {
    s_next_channel = (ledc_channel_t)((int)s_next_channel + 1);
    if ((int)s_next_channel % 2 == 0) {
        s_next_timer = (ledc_timer_t)((int)s_next_timer + 1);
    }
}

bool Servo360Driver::attach(int pin) {
    if (is_attached_) return true;

    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.duty_resolution = LEDC_TIMER_14_BIT;
    timer_conf.timer_num = ledc_timer_;
    timer_conf.freq_hz = SERVO_PWM_FREQ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer_conf) != ESP_OK) return false;

    ledc_channel_config_t chan_conf = {};
    chan_conf.gpio_num = pin;
    chan_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    chan_conf.channel = ledc_channel_;
    chan_conf.intr_type = LEDC_INTR_DISABLE;
    chan_conf.timer_sel = ledc_timer_;
    chan_conf.duty = 0;
    chan_conf.hpoint = 0;
    if (ledc_channel_config(&chan_conf) != ESP_OK) return false;

    pin_ = pin;
    is_attached_ = true;
    stop();
    return true;
}

void Servo360Driver::detach() {
    if (!is_attached_) return;
    ledc_stop(LEDC_LOW_SPEED_MODE, ledc_channel_, 0);
    is_attached_ = false;
    pin_ = -1;
}

bool Servo360Driver::attached() const { return is_attached_; }
int Servo360Driver::pin() const { return pin_; }

void Servo360Driver::set_speed_percent(int speed_percent) {
    if (!is_attached_) return;
    if (speed_percent < -100) speed_percent = -100;
    if (speed_percent > 100) speed_percent = 100;

    current_speed_percent_ = speed_percent;

    int range_neg = stop_pulse_us_ - min_pulse_us_;
    int range_pos = max_pulse_us_ - stop_pulse_us_;
    int pulse_us = stop_pulse_us_;

    if (speed_percent < 0) {
        pulse_us = stop_pulse_us_ + (speed_percent * range_neg) / 100;
    } else if (speed_percent > 0) {
        pulse_us = stop_pulse_us_ + (speed_percent * range_pos) / 100;
    }

    set_pulse_us(pulse_us);
}

void Servo360Driver::set_pulse_us(int pulse_us) {
    if (!is_attached_) return;
    if (pulse_us < min_pulse_us_) pulse_us = min_pulse_us_;
    if (pulse_us > max_pulse_us_) pulse_us = max_pulse_us_;
    uint32_t duty = pulse_to_duty(pulse_us);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
}

void Servo360Driver::stop() {
    current_speed_percent_ = 0;
    set_pulse_us(stop_pulse_us_);
}

int Servo360Driver::current_speed_percent() const { return current_speed_percent_; }
int Servo360Driver::stop_pulse_us() const { return stop_pulse_us_; }

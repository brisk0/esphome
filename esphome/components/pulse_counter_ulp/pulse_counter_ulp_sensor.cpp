#include "pulse_counter_ulp_sensor.h"
#include "esphome/core/log.h"
#include "esp32/ulp.h"
#include "ulp_main.h"
#include "soc/rtc_periph.h"
#include "driver/rtc_io.h"
#include <esp_sleep.h>

namespace esphome {
namespace pulse_counter_ulp {

static const char *const TAG = "pulse_counter_ulp";

const char *to_string(CountMode count_mode) {
  switch (count_mode) {
    case CountMode::disable:
      return "disable";
    case CountMode::increment:
      return "increment";
    case CountMode::decrement:
      return "decrement";
  }
  return "UNKNOWN MODE";
}

/* === ULP === */

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

std::unique_ptr<UlpProgram> UlpProgram::start(const Config &config) {
  esp_err_t error = ulp_load_binary(0, ulp_main_bin_start, (ulp_main_bin_end - ulp_main_bin_start) / sizeof(uint32_t));
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "Loading ULP binary failed: %s", esp_err_to_name(error));
    return nullptr;
  }

  /* GPIO used for pulse counting. */
  gpio_num_t gpio_num = static_cast<gpio_num_t>(config.pin_->get_pin());
  int rtcio_num = rtc_io_number_get(gpio_num);
  if (!rtc_gpio_is_valid_gpio(gpio_num)) {
    ESP_LOGE(TAG, "GPIO used for pulse counting must be an RTC IO");
  }

  /* Initialize some variables used by ULP program.
   * Each 'ulp_xyz' variable corresponds to 'xyz' variable in the ULP program.
   * These variables are declared in an auto generated header file,
   * 'ulp_main.h', name of this file is defined in component.mk as ULP_APP_NAME.
   * These variables are located in RTC_SLOW_MEM and can be accessed both by the
   * ULP and the main CPUs.
   *
   * Note that the ULP reads only the lower 16 bits of these variables.
   */
  ulp_edge_count = 0;
  ulp_run_count = 0;
  ulp_debounce_counter = 3;
  ulp_debounce_max_count = config.debounce_;
  ulp_next_edge = 0;
  ulp_io_number = rtcio_num; /* map from GPIO# to RTC_IO# */
  ulp_mean_exec_time = config.sleep_duration_ / microseconds{1};

  /* Initialize selected GPIO as RTC IO, enable input */
  rtc_gpio_init(gpio_num);
  rtc_gpio_set_direction(gpio_num, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_hold_en(gpio_num);

  /* Set ULP wake up period T
   * Minimum pulse width has to be T * (ulp_debounce_counter + 1).
   */
  ulp_set_wakeup_period(0, config.sleep_duration_ / std::chrono::microseconds{1});

  /* Start the program */
  error = ulp_run(&ulp_entry - RTC_SLOW_MEM);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "Starting ULP program failed: %s", esp_err_to_name(error));
    return nullptr;
  }

  return std::unique_ptr<UlpProgram>(new UlpProgram());
}

UlpProgram::State UlpProgram::pop_state() {
  // TODO count edges separately
  State state = UlpProgram::peek_state();
  ulp_edge_count = 0;
  ulp_run_count = 0;
  return state;
}

UlpProgram::State UlpProgram::peek_state() const {
  auto edge_count = static_cast<uint16_t>(ulp_edge_count);
  auto run_count = static_cast<uint16_t>(ulp_run_count);
  auto mean_exec_time = microseconds{1} * static_cast<uint16_t>(ulp_mean_exec_time);
  return {.edge_count = edge_count, .run_count = run_count, .mean_exec_time = mean_exec_time};
}

void UlpProgram::set_mean_exec_time(microseconds mean_exec_time) {
  ulp_mean_exec_time = static_cast<uint16_t>(mean_exec_time / microseconds{1});
}

/* === END ULP ===*/

void PulseCounterUlpSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up pulse counter '%s'...", this->name_.c_str());

  this->config_.pin_->setup();

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    ESP_LOGD(TAG, "Did not wake up from sleep, assuming restart or first boot and setting up ULP program");
    this->storage_ = UlpProgram::start(this->config_);
  } else {
    ESP_LOGD(TAG, "Woke up from sleep, skipping set-up of ULP program");
    this->storage_ = std::unique_ptr<UlpProgram>(new UlpProgram);
    UlpProgram::State state = this->storage_->peek_state();
    this->last_time_ = clock::now() - state.run_count * state.mean_exec_time;
  }

  if (!this->storage_) {
    this->mark_failed();
    return;
  }
}

void PulseCounterUlpSensor::dump_config() {
  LOG_SENSOR("", "Pulse Counter", this);
  LOG_PIN("  Pin: ", this->config_.pin_);
  ESP_LOGCONFIG(TAG, "  Rising Edge: %s", to_string(this->config_.rising_edge_mode_));
  ESP_LOGCONFIG(TAG, "  Falling Edge: %s", to_string(this->config_.falling_edge_mode_));
  ESP_LOGCONFIG(TAG, "  Sleep Duration: " PRIu32 " µs", this->config_.sleep_duration_ / microseconds{1});
  ESP_LOGCONFIG(TAG, "  Debounce: " PRIu16, this->config_.debounce_);
  LOG_UPDATE_INTERVAL(this);
}

void PulseCounterUlpSensor::update() {
  // Can't update if ulp program hasn't been initialised
  if (!this->storage_) {
    return;
  }
  UlpProgram::State raw = this->storage_->pop_state();
  clock::time_point now = clock::now();
  clock::duration interval = now - this->last_time_;
  if (interval != clock::duration::zero()) {
    this->storage_->set_mean_exec_time(std::chrono::duration_cast<microseconds>(interval / raw.run_count));
    float value = std::chrono::minutes{1} * static_cast<float>(raw.edge_count) / interval;  // pulses per minute
    ESP_LOGD(TAG, "'%s': Retrieved counter: %0.2f pulses/min", this->get_name().c_str(), value);
    this->publish_state(value);
  }

  this->last_time_ = now;
}

}  // namespace pulse_counter_ulp
}  // namespace esphome

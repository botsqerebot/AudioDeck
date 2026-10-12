// power.h — power off (deep sleep), power on with a long Play press, status LEDs.
//
//  Off:      hold Home 5 s  -> "Powering off", analog regulator, amp and DAC off, deep sleep
//  On:       hold Play 5 s  -> green LED blinks while you hold, then the player starts
//  LEDs:     green = on, orange = charging (also while off), red = low battery,
//            purple and blue are kept off
//
// While off the ESP32 sleeps. If the charger is plugged in it wakes for a moment every
// 30 s to update the orange LED (BQ24074 CHG pin), otherwise it only wakes when Play is
// pressed or the charger is plugged in.
#pragma once
#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include "config.h"

namespace Power {

static bool redLatched = false;
static uint32_t lastUpdate = 0;

static void led(int pin, bool on) {
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

static bool charging() { return digitalRead(PIN_CHG_STAT) == LOW; }     // BQ24074 CHG: low while charging
static bool pluggedIn() { return digitalRead(PIN_CHG_PGOOD) == LOW; }   // BQ24074 PGOOD: low with input power

// Battery voltage in volts, 0 if no battery sense pin is configured
static float batteryVolts() {
  if (PIN_BAT_ADC < 0) return 0;
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT_ADC);
  return mv / 8.0f / 1000.0f * BAT_DIVIDER;
}

static void releaseHolds() {
  const int pins[] = {PIN_LDO_EN, PIN_AMP_SHDN, PIN_DAC_XSMT, PIN_LED_GREEN, PIN_LED_PURPLE, PIN_LED_ORANGE, PIN_LED_BLUE, PIN_LED_RED};
  for (int p : pins) if (p >= 0) gpio_hold_dis((gpio_num_t)p);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit((gpio_num_t)PIN_BTN_PLAY);
  rtc_gpio_deinit((gpio_num_t)PIN_CHG_PGOOD);
}

static void holdLow(int pin) {
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  gpio_hold_en((gpio_num_t)pin);
}

// Everything off, then deep sleep. Never returns (the next start is a fresh boot).
[[noreturn]] static void sleepNow() {
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);
  pinMode(PIN_CHG_PGOOD, INPUT_PULLUP);
  delay(2);
  bool chg = charging(), plugged = pluggedIn();

  holdLow(PIN_DAC_XSMT);       // DAC muted
  holdLow(PIN_AMP_SHDN);       // amp shut down
  holdLow(PIN_LDO_EN);         // analog regulator off
  led(PIN_LED_GREEN, false);
  led(PIN_LED_PURPLE, false);
  led(PIN_LED_BLUE, false);
  led(PIN_LED_RED, false);
  led(PIN_LED_ORANGE, chg);
  const int leds[] = {PIN_LED_GREEN, PIN_LED_PURPLE, PIN_LED_ORANGE, PIN_LED_BLUE, PIN_LED_RED};
  for (int p : leds) if (p >= 0) gpio_hold_en((gpio_num_t)p);
  gpio_deep_sleep_hold_en();

  // wake on Play (held for 5 s is checked after waking)
  rtc_gpio_init((gpio_num_t)PIN_BTN_PLAY);
  rtc_gpio_set_direction((gpio_num_t)PIN_BTN_PLAY, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_PLAY);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_PLAY);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_PLAY, 0);

  if (plugged) {
    esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_CHECK_SEC * 1000000ULL);   // keep the orange LED right
  } else {
    rtc_gpio_init((gpio_num_t)PIN_CHG_PGOOD);                                 // wake when the charger is plugged in
    rtc_gpio_set_direction((gpio_num_t)PIN_CHG_PGOOD, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_CHG_PGOOD);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_CHG_PGOOD);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_CHG_PGOOD, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  Serial.flush();
  esp_deep_sleep_start();
}

// First thing in setup(). Returns only if the player should start.
static void handleWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  releaseHolds();
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);
  pinMode(PIN_CHG_PGOOD, INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  // keep the analog side off until we know we're starting
  pinMode(PIN_LDO_EN, OUTPUT);   digitalWrite(PIN_LDO_EN, LOW);
  pinMode(PIN_AMP_SHDN, OUTPUT); digitalWrite(PIN_AMP_SHDN, LOW);
  pinMode(PIN_DAC_XSMT, OUTPUT); digitalWrite(PIN_DAC_XSMT, LOW);
  led(PIN_LED_PURPLE, false);
  led(PIN_LED_BLUE, false);
  led(PIN_LED_RED, false);
  led(PIN_LED_ORANGE, charging());

  if (cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_EXT1) sleepNow();   // charger check only

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // Play must stay down for POWER_ON_HOLD_MS (counted from the wake-up press)
    uint32_t t0 = millis();
    while (millis() < POWER_ON_HOLD_MS) {
      if (digitalRead(PIN_BTN_PLAY) != LOW) {          // let go too early: back to sleep
        if (millis() - t0 > 30) sleepNow();
      } else t0 = millis();
      led(PIN_LED_GREEN, (millis() / 150) % 2);       // blink while counting
      delay(10);
    }
  }
  // Power-on reset (battery connected, USB, reset button) starts straight away
  led(PIN_LED_GREEN, true);
}

// Call from loop(): LEDs follow the charger and battery
static void update() {
  uint32_t now = millis();
  if (now - lastUpdate < 1000) return;
  lastUpdate = now;
  led(PIN_LED_GREEN, true);
  led(PIN_LED_ORANGE, charging());
  float v = batteryVolts();
  if (v > 0.5f) {
    if (!redLatched && v < BAT_LOW_V) redLatched = true;
    else if (redLatched && v > BAT_LOW_V + 0.10f) redLatched = false;   // hysteresis, no flicker
  }
  led(PIN_LED_RED, redLatched);
}

}  // namespace Power

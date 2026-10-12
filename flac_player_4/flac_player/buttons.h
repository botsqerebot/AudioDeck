// buttons.h — debounced buttons with press, short, long and auto-repeat events
//
// One physical press = exactly one EV_PRESS:
//  - a press must be stable for BTN_PRESS_MS and a release for BTN_RELEASE_MS, so contact
//    chatter on release can't register as a second press
//  - if the loop was stalled, the stalled time doesn't count as "held", so a normal
//    press can't turn into a repeat or a long press
#pragma once
#include <Arduino.h>
#include "config.h"

enum : uint8_t { EV_PRESS = 1, EV_SHORT = 2, EV_LONG = 4, EV_REPEAT = 8 };

struct Button {
  uint8_t pin = 0;
  const char *name = "";
  bool down = false, raw = false, longFired = false, ignore = false;
  uint32_t t0 = 0, lastChange = 0, lastRep = 0, lastPoll = 0;

  void begin(uint8_t p, const char *n) {
    pin = p; name = n; pinMode(p, INPUT_PULLUP);
    delay(1);
    ignore = digitalRead(pin) == (BUTTON_ACTIVE_LOW ? LOW : HIGH);   // still held from power-on: skip this press
  }

  uint8_t poll(uint32_t now) {
    bool r = digitalRead(pin) == (BUTTON_ACTIVE_LOW ? LOW : HIGH);
    uint8_t ev = 0;
    uint32_t gap = now - lastPoll;
    lastPoll = now;
    if (down && gap > 40) { t0 += gap; lastRep += gap; }      // stalled loop: don't count as held

    if (r != raw) { raw = r; lastChange = now; }
    if (ignore) {                                         // wait for the release, no events
      if (!r && now - lastChange >= BTN_RELEASE_MS) ignore = false;
      return 0;
    }
    uint32_t stable = now - lastChange;
    if (r != down && stable >= (r ? BTN_PRESS_MS : BTN_RELEASE_MS)) {
      down = r;
      if (down) { t0 = lastRep = now; longFired = false; ev |= EV_PRESS; }
      else if (!longFired) ev |= EV_SHORT;
    }
    if (down) {
      if (!longFired && now - t0 >= LONG_PRESS_MS) { longFired = true; ev |= EV_LONG; }
      if (now - t0 >= REPEAT_DELAY_MS && now - lastRep >= REPEAT_RATE_MS) { lastRep = now; ev |= EV_REPEAT; }
    }
#if BUTTON_DEBUG
    if (ev) Serial.printf("[btn] %s%s%s%s%s\n", name, ev & EV_PRESS ? " press" : "", ev & EV_SHORT ? " short" : "",
                          ev & EV_LONG ? " long" : "", ev & EV_REPEAT ? " repeat" : "");
#endif
    return ev;
  }
};

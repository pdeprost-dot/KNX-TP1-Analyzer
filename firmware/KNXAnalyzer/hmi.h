#pragma once
#include <Arduino.h>
class Arduino_GFX;

namespace hmi {
void begin(Arduino_GFX *display);
void tick();
void refresh();
}

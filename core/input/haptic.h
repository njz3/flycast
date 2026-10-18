/*
	Copyright 2024 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "types.h"
#include "cfg/option.h"
#include <algorithm>

#ifdef USE_SDL
void sdl_setTorque(int, float);
void sdl_setDamper(int, float, float);
void sdl_setSpring(int, float, float, float);
void sdl_setSine(int, float, float, u32);
void sdl_stopHaptic(int);
#endif

namespace haptic {

// The user's force feedback gain. The effects are capped at the maximum force of the device.
inline static float applyGain(float v) {
	return std::clamp(v * config::ForceFeedbackGain / 100.f, -1.f, 1.f);
}

inline static void setTorque(int port, float v) {
#ifdef USE_SDL
	sdl_setTorque(port, applyGain(v));
#endif
}

// A stronger damper or spring has both a higher maximum force and a steeper slope
inline static void setDamper(int port, float param, float speed) {
#ifdef USE_SDL
	sdl_setDamper(port, applyGain(param), applyGain(speed));
#endif
}

// center: the position the spring pulls to, from -1 (full left) to 1 (full right)
inline static void setSpring(int port, float saturation, float speed, float center = 0.f) {
#ifdef USE_SDL
	sdl_setSpring(port, applyGain(saturation), applyGain(speed), center);
#endif
}

inline static void setSine(int port, float power, float frequency, u32 duration_ms) {
#ifdef USE_SDL
	sdl_setSine(port, applyGain(power), frequency, duration_ms);
#endif
}

inline static void stopAll(int port) {
#ifdef USE_SDL
	sdl_stopHaptic(port);
#endif
}

}

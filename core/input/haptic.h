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
#ifndef LIBRETRO
#include "ui/agent.h"
#endif
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
#ifndef LIBRETRO
	agent::forceFeedback(port, "constant", v);
#endif
#ifdef USE_SDL
	sdl_setTorque(port, applyGain(v));
#endif
}

// A stronger damper or spring has both a higher maximum force and a steeper slope
inline static void setDamper(int port, float param, float speed) {
#ifndef LIBRETRO
	agent::forceFeedback(port, "damper", param);
	agent::forceFeedback(port, "damper.slope", speed);
#endif
#ifdef USE_SDL
	sdl_setDamper(port, applyGain(param), applyGain(speed));
#endif
}

// center: the position the spring pulls to, from -1 (full left) to 1 (full right)
inline static void setSpring(int port, float saturation, float speed, float center = 0.f) {
#ifndef LIBRETRO
	agent::forceFeedback(port, "selfcenter", saturation);
	agent::forceFeedback(port, "selfcenter.slope", speed);
	agent::forceFeedback(port, "selfcenter.center", center);
#endif
#ifdef USE_SDL
	sdl_setSpring(port, applyGain(saturation), applyGain(speed), center);
#endif
}

inline static void setSine(int port, float power, float frequency, u32 duration_ms) {
#ifndef LIBRETRO
	agent::forceFeedback(port, "vibrate", power);
	agent::forceFeedback(port, "vibrate.frequency", frequency);
	agent::forceFeedback(port, "vibrate.duration", (float)duration_ms);
#endif
#ifdef USE_SDL
	sdl_setSine(port, applyGain(power), frequency, duration_ms);
#endif
}

inline static void stopAll(int port) {
#ifndef LIBRETRO
	agent::forceFeedback(port, "stop", 0.f);
#endif
#ifdef USE_SDL
	sdl_stopHaptic(port);
#endif
}

}

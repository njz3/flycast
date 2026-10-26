/*
	Copyright 2026 njz3

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

struct MapleInputState;

// Agent mode: Flycast driven frame by frame by another program, through stdin and stdout,
// with the protocol of Supermodel's and sm2-emu's agent modes (described in agent.cpp)
namespace agent
{

// Takes the agent options out of the command line, and stdout for the protocol. Before anything else
void init(int& argc, char *argv[]);
// The configuration the agent mode needs. After the command line is parsed
void configure();

bool enabled();
// -agent: the window is hidden (not with -agent-show)
bool hidden();

// The held inputs over the devices' at each maple DMA
void applyInputs(MapleInputState state[4]);
// Main loop: true when a frame must run now, false while waiting for a command
bool beforeFrame();
void afterFrame();

// What the game sends to the outputs and to the force feedback
void output(const char *name, u32 value);
void forceFeedback(int port, const char *effect, float value);

}

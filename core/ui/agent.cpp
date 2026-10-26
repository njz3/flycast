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

/*
 * Agent mode: Flycast driven frame by frame by another program, with the protocol of Supermodel's and
 * sm2-emu's agent modes, so that one client drives the three emulators.
 *
 *   flycast -agent [-no-throttle] [-load-state FILE] [options] ROM
 *
 * -agent hides the window and mutes the sound; -agent-show shows them, to watch the game played by the
 * program. The game runs in real time unless -no-throttle is given, then as fast as possible.
 * -load-state starts from a save state, loaded before the first frame.
 *
 * One command a line on stdin, one answer on stdout (everything else Flycast prints goes to stderr).
 * Once the game is loaded:
 *
 *   ready GAME WIDTH HEIGHT
 *
 * GAME is the name of the ROM file without its extension, WIDTH x HEIGHT the size of obs's frames.
 * Then, for each command:
 *
 *   step [N]            Runs N frames (vertical blanks, 1 by default) with the inputs as set.
 *                       Answer: "ok FRAME" then, on the same line, the bytes the game sent to its drive
 *                       board meanwhile, in hex (m3ffb and awffb, 3 bytes for a midiffb message); a value
 *                       equal to the previous one is not repeated.
 *   set INPUT VALUE     Holds an input at a value until released: an axis (Steering...) from -1 to 1,
 *                       +1 at its left (or top) end as a wheel's angle, a pedal (Accelerator, Brake...)
 *                       from 0 to 1, a button (Start1, Coin1, TestA...) 0 or 1. Answer: "ok" or "err".
 *   setraw INPUT VALUE  The same with the raw value: 0 to 65535 for an axis (0 at its left end) or a
 *                       pedal, 0 or 1 for a button.
 *   get INPUT           The raw value the game reads (held, or from the devices): "value RAW".
 *   release INPUT|all   Gives the input back to the devices.
 *   inputs              "inputs N", then N lines "NAME TYPE", TYPE axis, analog or switch: the game's
 *                       own names (HANDLE, ACCEL..., spaces as _), then Supermodel's (Steering,
 *                       Accelerator, Brake, Start1, Coin1, ServiceA, TestA, JoyUp, JoyButton1...,
 *                       with 2 or _2 for player 2). Service and Test are also TestA and ServiceA.
 *   obs [W H]           The last frame scaled to W x H (its own size if omitted or 0, black before the
 *                       first one): "obs W H", a new line, then W * H * 3 bytes, RGB, top row first.
 *                       Its own size: the height of the render resolution, the width of the display's
 *                       aspect ratio (given by ready).
 *   shot PATH           Saves the last frame, as a PNG file if PATH ends with .png, else BMP. "ok" or "err".
 *   save PATH           Saves a state. "ok" or "err".
 *   load PATH           Loads a state. "ok" or "err".
 *   ffb                 The force feedback commands sent since the last call, whether a device takes
 *                       them or not, before the gain: "ffb N", then N lines "FRAME INPUT EFFECT FORCE".
 *                       EFFECT constant (FORCE -1..1, sent as is to SDL, so FORCE * 10000 to DirectInput),
 *                       selfcenter (the spring's saturation, then selfcenter.slope and selfcenter.center,
 *                       -1 left to 1 right), damper (saturation, then damper.slope), vibrate (power, then
 *                       vibrate.frequency in Hz and vibrate.duration in ms) or stop.
 *   outputs             The outputs set so far: "outputs NAME=VALUE ...", in hex (lamp0..., m3ffb...).
 *   ram ADDRESS LENGTH  Memory read through the SH4's bus (hex address and length, at most 1 MB):
 *                       "ram LENGTH", a new line, the bytes.
 *   mem ADDRESS LENGTH  The same (sm2-emu's name): "mem LENGTH", the bytes.
 *   poke ADDRESS VALUE [SIZE]  Writes VALUE (hex, SIZE 1, 2 or 4 bytes, 4 by default) at ADDRESS: "ok".
 *   frame               "frame N", the number of frames run.
 *   settle [MS]         Waits MS milliseconds before each frame (0 by default): "settle MS".
 *   quit                "bye", and Flycast exits.
 *
 * A command that fails answers "err" and a reason. Supermodel's servo, tel, menu, shotwin and driveread
 * aren't available.
 */
#include "agent.h"
#include "emulator.h"
#include "mainui.h"
#include "stdclass.h"
#include "cfg/cfg.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_sched.h"
#include "rend/transform_matrix.h"
#include "input/gamepad.h"

#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace agent
{

namespace
{

bool agentEnabled;
bool agentShown;
bool throttle = true;
std::string loadStatePath;

FILE *out;		// the protocol's stdout
// stdin, read without the C library: a thread blocked on a FILE would keep Flycast from exiting
#ifdef _WIN32
HANDLE in = INVALID_HANDLE_VALUE;
#else
int in = -1;
#endif

// The commands are read by a thread, so that the window keeps processing its events while waiting
std::mutex queueMutex;
std::condition_variable queueCond;
std::deque<std::string> queue;
bool inputClosed;

bool ready;
bool stepping;
u64 frames;		// vertical blanks
u64 stepEnd;
int settleMs;

// Real time: the emulated time against the wall clock
bool paceStarted;
u64 paceSh4;
std::chrono::steady_clock::time_point paceWall;

std::mutex outputMutex;
std::map<std::string, u32> outputs;
std::vector<u8> driveBytes;
std::string lastDriveName;
u32 lastDriveValue;

struct FfbRecord
{
	u64 frame;
	int port;
	const char *effect;
	float value;
};
std::vector<FfbRecord> ffbRecords;

enum class InputType { Axis, Analog, Switch };

struct Input
{
	std::string name;
	InputType type;
	int port;
	u32 bit;	// switch: the bit in kcode
	int axis;	// axis: in fullAxes, analog: in halfAxes
	bool listed;
};
std::vector<Input> inputs;
std::vector<std::pair<const Input *, int>> holds;

void reply(const char *format, ...)
{
	if (out == nullptr)
		return;
	va_list args;
	va_start(args, format);
	vfprintf(out, format, args);
	va_end(args);
	fputc('\n', out);
	fflush(out);
}

void replyBytes(const std::string& header, const u8 *data, size_t size)
{
	if (out == nullptr)
		return;
	fputs(header.c_str(), out);
	fputc('\n', out);
	fwrite(data, 1, size, out);
	fflush(out);
}

// The protocol gets stdin and stdout; whatever else is written to stdout goes to stderr
void takeStdio()
{
	fflush(stdout);
#ifdef _WIN32
	const HANDLE process = GetCurrentProcess();
	HANDLE handle;
	if (DuplicateHandle(process, GetStdHandle(STD_OUTPUT_HANDLE), process, &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
	{
		int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
		if (fd >= 0)
			out = _fdopen(fd, "wb");
	}
	in = GetStdHandle(STD_INPUT_HANDLE);
	SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
	if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0)
		_dup2(_fileno(stderr), _fileno(stdout));
	if (out == nullptr || in == nullptr || in == INVALID_HANDLE_VALUE)
		fprintf(stderr, "Agent mode: no stdin or stdout\n");
#else
	int fd = dup(fileno(stdout));
	if (fd >= 0)
		out = fdopen(fd, "wb");
	in = fileno(stdin);
	dup2(fileno(stderr), fileno(stdout));
	if (out == nullptr)
		fprintf(stderr, "Agent mode: no stdout\n");
#endif
}

void readCommands()
{
	std::string pending;
	char buffer[4096];
	for (;;)
	{
#ifdef _WIN32
		DWORD size = 0;
		if (in == nullptr || in == INVALID_HANDLE_VALUE || !ReadFile(in, buffer, sizeof(buffer), &size, nullptr) || size == 0)
			break;
#else
		ssize_t size = read(in, buffer, sizeof(buffer));
		if (size <= 0)
			break;
#endif
		pending.append(buffer, size);
		size_t end;
		while ((end = pending.find('\n')) != std::string::npos)
		{
			std::string line = pending.substr(0, end);
			pending.erase(0, end + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			std::lock_guard<std::mutex> _(queueMutex);
			queue.push_back(std::move(line));
			queueCond.notify_one();
		}
	}
	std::lock_guard<std::mutex> _(queueMutex);
	inputClosed = true;
	queueCond.notify_one();
}

void onVBlank(Event, void *) {
	frames++;
}

/*
 * Inputs
 */

std::string inputName(const char *gameName)
{
	std::string name(gameName);
	std::replace(name.begin(), name.end(), ' ', '_');
	return name;
}

const Input *findInput(const std::string& name)
{
	for (const Input& input : inputs)
		if (input.name.size() == name.size()
				&& std::equal(name.begin(), name.end(), input.name.begin(),
						[](char a, char b) { return std::tolower((u8)a) == std::tolower((u8)b); }))
			return &input;
	return nullptr;
}

void addInput(const std::string& name, InputType type, int port, u32 bit, int axis, bool listed = true)
{
	// The first one of a name is kept: the game's own names come first. Start, coin, test and service
	// have no name of their own
	if (!name.empty() && findInput(name) == nullptr)
		inputs.push_back({ name, type, port, bit, axis, listed });
}

void buildInputs()
{
	inputs.clear();
	// The game's own names (Naomi, Atomiswave), for player 1
	for (u32 bit = 1; bit <= DC_BTN_BITMAPPED_LAST; bit <<= 1)
		if (const char *name = GetCurrentGameButtonName((DreamcastKey)bit))
			addInput(inputName(name), InputType::Switch, 0, bit, 0);
	static const struct {
		DreamcastKey key;
		InputType type;
		int axis;
	} gameAxes[] {
		{ DC_AXIS_LEFT, InputType::Axis, PJAI_X1 },
		{ DC_AXIS_UP, InputType::Axis, PJAI_Y1 },
		{ DC_AXIS2_LEFT, InputType::Axis, PJAI_X2 },
		{ DC_AXIS2_UP, InputType::Axis, PJAI_Y2 },
		{ DC_AXIS_RT, InputType::Analog, PJTI_R },
		{ DC_AXIS_LT, InputType::Analog, PJTI_L },
		{ DC_AXIS_RT2, InputType::Analog, PJTI_R2 },
		{ DC_AXIS_LT2, InputType::Analog, PJTI_L2 },
	};
	for (const auto& axis : gameAxes)
		if (const char *name = GetCurrentGameAxisName(axis.key))
			addInput(inputName(name), axis.type, 0, 0, axis.axis);

	// Supermodel's names
	addInput("Steering", InputType::Axis, 0, 0, PJAI_X1);
	addInput("AnalogJoyX", InputType::Axis, 0, 0, PJAI_X1, false);
	addInput("AnalogJoyY", InputType::Axis, 0, 0, PJAI_Y1);
	addInput("Accelerator", InputType::Analog, 0, 0, PJTI_R);
	addInput("Brake", InputType::Analog, 0, 0, PJTI_L);
	static const u32 joyButtons[] { DC_BTN_A, DC_BTN_B, DC_BTN_C, DC_BTN_X, DC_BTN_Y, DC_BTN_Z, DC_DPAD2_LEFT, DC_DPAD2_RIGHT };
	for (int port = 0; port < 2; port++)
	{
		const std::string player = std::to_string(port + 1);
		const std::string suffix = port == 0 ? "" : "2";
		addInput("Start" + player, InputType::Switch, port, DC_BTN_START, 0);
		addInput("Coin" + player, InputType::Switch, port, DC_BTN_D, 0);
		addInput(port == 0 ? "ServiceA" : "ServiceB", InputType::Switch, port, DC_DPAD2_UP, 0);
		addInput(port == 0 ? "TestA" : "TestB", InputType::Switch, port, DC_DPAD2_DOWN, 0);
		addInput("JoyUp" + suffix, InputType::Switch, port, DC_DPAD_UP, 0);
		addInput("JoyDown" + suffix, InputType::Switch, port, DC_DPAD_DOWN, 0);
		addInput("JoyLeft" + suffix, InputType::Switch, port, DC_DPAD_LEFT, 0);
		addInput("JoyRight" + suffix, InputType::Switch, port, DC_DPAD_RIGHT, 0);
		for (u32 i = 0; i < std::size(joyButtons); i++)
			addInput("JoyButton" + std::to_string(i + 1) + (port == 0 ? "" : "_2"), InputType::Switch, port, joyButtons[i], 0);
	}
	addInput("Service", InputType::Switch, 0, DC_DPAD2_UP, 0, false);
	addInput("Test", InputType::Switch, 0, DC_DPAD2_DOWN, 0, false);
}

int toRaw(const Input& input, float value)
{
	switch (input.type)
	{
	case InputType::Axis:
		// +1 at the left end (0), -1 at the right end
		return std::clamp((int)std::lround((1.f - std::clamp(value, -1.f, 1.f)) * 32767.5f), 0, 65535);
	case InputType::Analog:
		return (int)std::lround(std::clamp(value, 0.f, 1.f) * 65535.f);
	default:
		return value >= 0.5f ? 1 : 0;
	}
}

int readRaw(const Input& input, const MapleInputState& state)
{
	switch (input.type)
	{
	case InputType::Axis:
		return state.fullAxes[input.axis] + 32768;
	case InputType::Analog:
		return state.halfAxes[input.axis];
	default:
		return (state.kcode & input.bit) == 0 ? 1 : 0;
	}
}

void writeRaw(const Input& input, int raw, MapleInputState& state)
{
	switch (input.type)
	{
	case InputType::Axis:
		state.fullAxes[input.axis] = (int16_t)(std::clamp(raw, 0, 65535) - 32768);
		break;
	case InputType::Analog:
		state.halfAxes[input.axis] = (u16)std::clamp(raw, 0, 65535);
		break;
	default:
		if (raw != 0)
			state.kcode &= ~input.bit;
		else
			state.kcode |= input.bit;
		break;
	}
}

void hold(const Input *input, int raw)
{
	for (auto& hold : holds)
		if (hold.first == input)
		{
			hold.second = raw;
			return;
		}
	holds.emplace_back(input, raw);
}

/*
 * Frames
 */

std::string gameName()
{
	std::string name = settings.content.path;
	size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos)
		name = name.substr(slash + 1);
	return get_file_basename(name);
}

// The last frame at the height of the render resolution
bool lastFrame(std::vector<u8>& rgb, int& width, int& height)
{
	rgb.clear();
	width = 0;
	height = config::RenderResolution;
	try {
		return renderer != nullptr && renderer->GetLastFrame(rgb, width, height)
				&& width > 0 && height > 0 && width <= 16384 && height <= 16384
				&& rgb.size() >= (size_t)width * height * 3;
	} catch (const std::bad_alloc&) {
		// Seen on F355 twin's master (multiboard): black instead
		rgb.clear();
		return false;
	}
}

// Box average down (or nearest up) to width x height
std::vector<u8> scale(const std::vector<u8>& rgb, int srcWidth, int srcHeight, int width, int height)
{
	std::vector<u8> scaled((size_t)width * height * 3);
	for (int y = 0; y < height; y++)
	{
		const int y0 = y * srcHeight / height;
		const int y1 = std::max(y0 + 1, (y + 1) * srcHeight / height);
		for (int x = 0; x < width; x++)
		{
			const int x0 = x * srcWidth / width;
			const int x1 = std::max(x0 + 1, (x + 1) * srcWidth / width);
			u32 sum[3] {};
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++)
					for (int c = 0; c < 3; c++)
						sum[c] += rgb[((size_t)sy * srcWidth + sx) * 3 + c];
			const u32 count = (y1 - y0) * (x1 - x0);
			for (int c = 0; c < 3; c++)
				scaled[((size_t)y * width + x) * 3 + c] = (u8)(sum[c] / count);
		}
	}
	return scaled;
}

void appendData(void *context, void *data, int size)
{
	std::vector<u8>& v = *(std::vector<u8> *)context;
	v.insert(v.end(), (u8 *)data, (u8 *)data + size);
}

bool writeImage(const std::string& path, const std::vector<u8>& rgb, int width, int height)
{
	std::vector<u8> file;
	if (get_file_extension(path) == "png")
		stbi_write_png_to_func(appendData, &file, width, height, 3, rgb.data(), width * 3);
	else
		stbi_write_bmp_to_func(appendData, &file, width, height, 3, rgb.data());
	FILE *f = nowide::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = !file.empty() && fwrite(file.data(), 1, file.size(), f) == file.size();
	return fclose(f) == 0 && ok;
}

void pace()
{
	if (!throttle)
		return;
	const u64 now = sh4_sched_now64();
	const auto wall = std::chrono::steady_clock::now();
	// From now on after a state is loaded, after waiting for a command or when late
	if (!paceStarted || now < paceSh4 || now - paceSh4 > SH4_MAIN_CLOCK)
	{
		paceStarted = true;
		paceSh4 = now;
		paceWall = wall;
		return;
	}
	const auto due = paceWall + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<double>((double)(now - paceSh4) / SH4_MAIN_CLOCK));
	if (due > wall)
		std::this_thread::sleep_until(due);
	else if (wall - due > std::chrono::milliseconds(100))
		paceStarted = false;
}

void start()
{
	ready = true;
	frames = 0;
	if (!loadStatePath.empty() && !dc_loadstate(loadStatePath))
		fprintf(stderr, "Agent mode: cannot load the state %s\n", loadStatePath.c_str());
	// No speed limit, no sound
	settings.input.fastForwardMode = !throttle;
	if (!agentShown)
		settings.aica.muteAudio = true;
	buildInputs();
	// The size of obs's frames, before any is rendered
	const int height = config::RenderResolution;
	const int width = (int)(getOutputFramebufferAspectRatio() * height);
	reply("ready %s %d %d", gameName().c_str(), width, height);
}

void stepDone()
{
	stepping = false;
	std::string answer = "ok " + std::to_string(frames);
	{
		std::lock_guard<std::mutex> _(outputMutex);
		char hex[4];
		for (u8 b : driveBytes)
		{
			snprintf(hex, sizeof(hex), " %02X", b);
			answer += hex;
		}
		driveBytes.clear();
	}
	reply("%s", answer.c_str());
}

/*
 * Commands
 */

// Numbers with a dot whatever the locale (Flycast sets the user's)
float parseFloat(const std::string& s)
{
	float value;
	const char *end = s.data() + s.size();
	auto result = std::from_chars(s.data(), end, value);
	if (result.ec != std::errc() || result.ptr != end)
		throw std::invalid_argument("not a number: " + s);
	return value;
}

std::string formatFloat(float value)
{
	char s[32];
	auto result = std::to_chars(s, s + sizeof(s), value, std::chars_format::fixed, 4);
	return std::string(s, result.ptr);
}

bool parseHex(const std::string& s, u32& value)
{
	if (s.empty())
		return false;
	char *end;
	value = (u32)strtoul(s.c_str(), &end, 16);
	return *end == '\0';
}

void quit()
{
	reply("bye");
	stepping = false;
	dc_exit();
}

void execute(const std::string& line)
{
	std::istringstream args(line);
	std::string command;
	args >> command;
	// The rest of the line, for a path
	auto rest = [&args]() {
		std::string s;
		std::getline(args >> std::ws, s);
		return s;
	};

	if (command == "step")
	{
		long long n = 1;
		args >> n;
		stepEnd = frames + std::max(n, 1ll);
		stepping = true;
	}
	else if (command == "set" || command == "setraw")
	{
		std::string name;
		std::string value;
		args >> name >> value;
		const Input *input = findInput(name);
		if (name.empty() || value.empty())
			reply("err usage: %s INPUT VALUE", command.c_str());
		else if (input == nullptr)
			reply("err unknown input %s", name.c_str());
		else
		{
			hold(input, command == "set" ? toRaw(*input, parseFloat(value)) : (int)std::stol(value, nullptr, 0));
			reply("ok");
		}
	}
	else if (command == "get")
	{
		std::string name;
		args >> name;
		const Input *input = findInput(name);
		if (input == nullptr)
			reply("err unknown input %s", name.c_str());
		else
		{
			int raw = readRaw(*input, mapleInputState[input->port]);
			for (const auto& hold : holds)
				if (hold.first == input)
					raw = hold.second;
			reply("value %d", raw);
		}
	}
	else if (command == "release")
	{
		std::string name;
		args >> name;
		if (name == "all")
			holds.clear();
		else if (const Input *input = findInput(name))
			holds.erase(std::remove_if(holds.begin(), holds.end(), [input](const auto& hold) { return hold.first == input; }),
					holds.end());
		reply("ok");
	}
	else if (command == "inputs")
	{
		static const char * const types[] { "axis", "analog", "switch" };
		const size_t count = std::count_if(inputs.begin(), inputs.end(), [](const Input& input) { return input.listed; });
		std::string answer = "inputs " + std::to_string(count);
		for (const Input& input : inputs)
			if (input.listed)
				answer += "\n" + input.name + " " + types[(int)input.type];
		reply("%s", answer.c_str());
	}
	else if (command == "obs")
	{
		int width = 0;
		int height = 0;
		args >> width >> height;
		std::vector<u8> rgb;
		int srcWidth, srcHeight;
		if (!lastFrame(rgb, srcWidth, srcHeight))
		{
			// Nothing rendered yet: black, at the size given by ready
			srcHeight = config::RenderResolution;
			srcWidth = (int)(getOutputFramebufferAspectRatio() * srcHeight);
			if (srcWidth <= 0 || srcWidth > 16384)
				srcWidth = srcHeight * 4 / 3;
			rgb.assign((size_t)srcWidth * srcHeight * 3, 0);
		}
		if (width <= 0 || height <= 0)
		{
			width = srcWidth;
			height = srcHeight;
		}
		std::vector<u8> scaled = scale(rgb, srcWidth, srcHeight, width, height);
		replyBytes("obs " + std::to_string(width) + " " + std::to_string(height), scaled.data(), scaled.size());
	}
	else if (command == "shot")
	{
		const std::string path = rest();
		std::vector<u8> rgb;
		int width, height;
		if (path.empty() || !lastFrame(rgb, width, height))
			reply("err no path or no frame");
		else if (!writeImage(path, rgb, width, height))
			reply("err cannot write %s", path.c_str());
		else
			reply("ok");
	}
	else if (command == "save" || command == "load")
	{
		const std::string path = rest();
		if (path.empty())
			reply("err usage: %s PATH", command.c_str());
		else if (command == "save" ? !dc_savestate(path) : !dc_loadstate(path))
			reply("err cannot %s %s", command.c_str(), path.c_str());
		else
		{
			if (command == "load")
			{
				std::lock_guard<std::mutex> _(outputMutex);
				driveBytes.clear();
				lastDriveName.clear();
				paceStarted = false;
			}
			reply("ok");
		}
	}
	else if (command == "ffb")
	{
		std::vector<FfbRecord> records;
		{
			std::lock_guard<std::mutex> _(outputMutex);
			records.swap(ffbRecords);
		}
		std::string answer = "ffb " + std::to_string(records.size());
		char line[128];
		for (const FfbRecord& record : records)
		{
			const std::string input = record.port == 0 ? "Steering" : "Steering" + std::to_string(record.port + 1);
			snprintf(line, sizeof(line), "\n%llu %s %s %s", (unsigned long long)record.frame, input.c_str(),
					record.effect, formatFloat(record.value).c_str());
			answer += line;
		}
		reply("%s", answer.c_str());
	}
	else if (command == "outputs")
	{
		std::string answer = "outputs";
		std::lock_guard<std::mutex> _(outputMutex);
		char value[16];
		for (const auto& [name, v] : outputs)
		{
			snprintf(value, sizeof(value), "=%02X", v);
			answer += " " + name + value;
		}
		reply("%s", answer.c_str());
	}
	else if (command == "ram" || command == "mem")
	{
		std::string a, l;
		args >> a >> l;
		u32 address, length;
		if (!parseHex(a, address) || !parseHex(l, length) || length > 0x100000)
			reply("err usage: %s ADDRESS LENGTH (hex, at most 100000)", command.c_str());
		else
		{
			std::vector<u8> data(length);
			for (u32 i = 0; i < length; i++)
				data[i] = addrspace::readt<u8>(address + i);
			replyBytes(command + " " + std::to_string(length), data.data(), data.size());
		}
	}
	else if (command == "poke")
	{
		std::string a, v;
		int size = 4;
		args >> a >> v >> size;
		u32 address, value;
		if (!parseHex(a, address) || !parseHex(v, value) || (size != 1 && size != 2 && size != 4))
			reply("err usage: poke ADDRESS VALUE [SIZE] (hex, SIZE 1, 2 or 4)");
		else
		{
			if (size == 1)
				addrspace::writet<u8>(address, (u8)value);
			else if (size == 2)
				addrspace::writet<u16>(address, (u16)value);
			else
				addrspace::writet<u32>(address, value);
			reply("ok");
		}
	}
	else if (command == "frame")
	{
		reply("frame %llu", (unsigned long long)frames);
	}
	else if (command == "settle")
	{
		args >> settleMs;
		settleMs = std::clamp(settleMs, 0, 1000);
		reply("settle %d", settleMs);
	}
	else if (command == "quit")
	{
		quit();
	}
	else if (command == "servo" || command == "tel" || command == "menu" || command == "shotwin" || command == "driveread")
	{
		reply("err %s is not available in Flycast", command.c_str());
	}
	else
	{
		reply("err unknown command %s", command.c_str());
	}
}

}	// anonymous namespace

void init(int& argc, char *argv[])
{
	int kept = 1;
	for (int i = 1; i < argc; i++)
	{
		const char *arg = argv[i];
		// -option or --option
		if (arg[0] == '-' && arg[1] == '-')
			arg++;
		if (!strcmp(arg, "-agent"))
			agentEnabled = true;
		else if (!strcmp(arg, "-agent-show"))
			agentEnabled = agentShown = true;
		else if (!strcmp(arg, "-no-throttle"))
			throttle = false;
		else if (!strcmp(arg, "-throttle"))
			throttle = true;
		else if (!strcmp(arg, "-load-state") && i + 1 < argc)
			loadStatePath = argv[++i];
		else if (!strncmp(arg, "-load-state=", 12))
			loadStatePath = arg + 12;
		else
			argv[kept++] = argv[i];
	}
	argc = kept;
	argv[argc] = nullptr;
	if (!agentEnabled)
		return;
	takeStdio();
	EventManager::listen(Event::VBlank, onVBlank);
	std::thread(readCommands).detach();
}

void configure()
{
	if (!agentEnabled)
		return;
	// Frame by frame on the main thread, nothing between two commands
	config::setTransient("config", "rend.ThreadedRendering", "no");
	config::setTransient("config", "rend.vsync", "no");
	config::setTransient("config", "Dreamcast.AutoLoadState", "no");
	config::setTransient("config", "Dreamcast.AutoSaveState", "no");
	if (!agentShown)
		config::setTransient("window", "fullscreen", "no");
}

bool enabled() {
	return agentEnabled;
}

bool hidden() {
	return agentEnabled && !agentShown;
}

void applyInputs(MapleInputState state[4])
{
	for (const auto& hold : holds)
		writeRaw(*hold.first, hold.second, state[hold.first->port]);
}

bool beforeFrame()
{
	if (!ready)
		start();
	while (!stepping)
	{
		std::string line;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			if (queue.empty() && !inputClosed)
				queueCond.wait_for(lock, std::chrono::milliseconds(10));
			if (queue.empty())
			{
				if (inputClosed)
				{
					// The client is gone
					lock.unlock();
					dc_exit();
				}
				return false;
			}
			line = std::move(queue.front());
			queue.pop_front();
		}
		try {
			execute(line);
		} catch (const std::exception& e) {
			// a value that isn't a number
			reply("err %s", e.what());
		}
		if (!emu.running())
			return false;
	}
	if (settleMs > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
	return true;
}

void afterFrame()
{
	pace();
	if (stepping && frames >= stepEnd)
		stepDone();
}

void output(const char *name, u32 value)
{
	if (!agentEnabled)
		return;
	std::lock_guard<std::mutex> _(outputMutex);
	outputs[name] = value;
	const bool midi = !strcmp(name, "midiffb");
	if (!midi && strcmp(name, "m3ffb") && strcmp(name, "awffb"))
		return;
	// A value equal to the previous one isn't repeated
	if (value == lastDriveValue && lastDriveName == name)
		return;
	lastDriveName = name;
	lastDriveValue = value;
	if (driveBytes.size() >= 65536)
		return;
	if (midi)
	{
		driveBytes.push_back((u8)(value >> 16));
		driveBytes.push_back((u8)(value >> 8));
	}
	driveBytes.push_back((u8)value);
}

void forceFeedback(int port, const char *effect, float value)
{
	if (!agentEnabled)
		return;
	std::lock_guard<std::mutex> _(outputMutex);
	if (ffbRecords.size() < 1000000)
		ffbRecords.push_back({ frames, port, effect, value });
}

}	// namespace agent

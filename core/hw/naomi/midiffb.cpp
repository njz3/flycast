/*
	Copyright 2025 flyinghead

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
#include "midiffb.h"
#include "naomi_cart.h"
#include "hw/aica/aica_if.h"
#include "hw/maple/maple_cfg.h"
#include "serialize.h"
#include "input/haptic.h"
#include "oslib/oslib.h"
#include "network/output.h"
#include "oslib/i18n.h"
#include "hw/sh4/sh4_sched.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

// Sega servo board 838-14174, driven through the AICA MIDI port
// (Initial D, Club Kart, King of Route 66, Sega Driving Simulator).
// The game sends 4-byte frames: a command >= 0x80, two 7-bit parameters and a checksum.
// The board answers every frame, from the address the FA command points to.
// Decoding from BackForceFeeder's DRIVEBOARD.md, measured on a real board.
// Thanks to njz3 and boomslangnz for their help
// https://www.arcade-projects.com/threads/force-feedback-translator-sega-midi-sega-rs422-and-namco-rs232.924/
namespace midiffb {

constexpr int PollPeriod = SH4_MAIN_CLOCK / 1000;	// 1 ms
constexpr u64 ShortModeTimeout = SH4_MAIN_CLOCK / 1000 * 240;	// 240 ms
constexpr u64 IndexSearchTime = SH4_MAIN_CLOCK / 1000 * 500;	// 500 ms

static bool initialized;
// Bidirectional network output: a client relays the frames to a real board and sends its answers back
static bool passthrough;
static int pollSchedId = -1;

static u8 midiTxBuf[4];
static u32 midiTxBufIndex;
// Set by a board reset (FF), ended when the force feedback is disabled (80 xx 00).
// The wheel position then follows the torque the game sends.
static bool calibrating;
static bool active;
// 80 01 xx: the torque drops 240 ms after the last frame
static bool shortMode;
static bool torqueDropped;
static u64 lastFrameTime;
static float power;
static float position = 8192.f;
static float centerOffset;
static int torque;	// from -128 (full right) to 127 (full left)
// The parameters of the commands 80-8F as last received, what the board answers at the addresses 00-0F
static u8 params[16][2];
// The address the board answers from (FA 00 aa). 1F gives one-byte statuses
static u8 replyAddress;
// Restored from a save state made by an older version, which always answered 90 with the wheel position
static bool legacyReplies;
enum BoardState : u8 { NotInitialized, SearchingIndex, Ready };
static u8 state;
static u64 searchEnd;
// 9D idx page selects a parameter of the effect table, which 9E MSB LSB writes
static u8 paramSelect[2];
static std::array<u16, 256> paramTable;

static void midiSend(u8 b1, u8 b2, u8 b3)
{
	aica::midiSend(b1);
	aica::midiSend(b2);
	aica::midiSend(b3);
	aica::midiSend((b1 ^ b2 ^ b3) & 0x7f);
}

// The encoder position, 14 bits, 0x2000 at the center set by F0
static int wheelPosition() {
	return (int)std::clamp(position - centerOffset, 0.f, 16383.f);
}

static void applyTorque()
{
	if (!active || calibrating)
		return;
	// 81: the maximum torque towards the center (b1) and away from it (b2)
	const int displacement = wheelPosition() - 0x2000;
	const bool towardsCenter = (torque > 0 && displacement < 0) || (torque < 0 && displacement > 0);
	const float limit = params[1][towardsCenter ? 0 : 1] / 127.f;
	haptic::setTorque(0, std::clamp(torque / 128.f, -limit, limit) * power);
}

static void applySpring()
{
	if (!active || calibrating)
		return;
	// 88 b1: 0 enabled, 1 disabled. 88 b2: the inverse of its gain (03 at power-up, 05 strong, 10 light)
	// 8B b1: its force (10 light, 20 strong)
	if (params[8][0] != 0 || params[0xb][0] == 0) {
		haptic::setSpring(0, 0.f, 0.f);
		return;
	}
	const float force = std::min(params[0xb][0] / 32.f, 1.f) * power;
	const float gain = std::min(3.f / std::max<int>(params[8][1], 1), 1.f);
	// 87: offset of the spring's center, like a torque (positive to the left), about 90 degrees at full scale
	const int offset = ((params[7][0] << 7) | params[7][1]) - 0x80;
	haptic::setSpring(0, force, gain, -offset / 128.f * 0.67f);
}

static void applyDamper()
{
	if (!active || calibrating)
		return;
	// 86: visco speed (b1) and param (b2), acting as their ratio: strong at 10 7F, light at 40 40, none when b2 is 0
	const float ratio = params[6][1] / (float)std::max<int>(params[6][0], 1);
	haptic::setDamper(0, ratio == 0.f ? 0.f : power, std::min(ratio / 8.f, 1.f));
}

static void applySine()
{
	if (!active)
		return;
	// 85: frequency in half Hz (b1) and amplitude (b2). Stopped when either is 0
	if (params[5][0] == 0 || params[5][1] == 0)
		haptic::setSine(0, 0.f, 0.f, 0);
	else
		haptic::setSine(0, params[5][1] / 127.f * power, params[5][0] / 2.f, 0xffffffff);	// until changed
}

static void applyEffects()
{
	applyTorque();
	applySpring();
	applyDamper();
	applySine();
}

// The board's state at power-up and after a reset (FF)
static void resetBoard()
{
	static const u8 PowerUpParams[16][2] {
		{ 0x00, 0x00 }, { 0x40, 0x7f }, { 0x04, 0x54 }, { 0x40, 0x04 },
		{ 0x01, 0x00 }, { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x01, 0x00 },
		{ 0x00, 0x03 }, { 0x00, 0x00 }, { 0x20, 0x40 }, { 0x00, 0x00 },
		{ 0x00, 0x00 }, { 0x01, 0x01 }, { 0x00, 0x01 }, { 0x00, 0x01 },
	};
	memcpy(params, PowerUpParams, sizeof(params));
	power = params[3][0] / 127.f;
	torque = 0;
	shortMode = false;
	torqueDropped = false;
	replyAddress = 0x1f;
	legacyReplies = false;
	state = NotInitialized;
}

// The answer to each frame, from the address set by FA
static void reply()
{
	if (legacyReplies)
	{
		const int pos = wheelPosition();
		midiSend(0x90, (pos >> 7) & 0x7f, pos & 0x7f);
		return;
	}
	// 80-9F once the index has been found, A0-BF before
	const u8 base = state == Ready ? 0x80 : 0xa0;
	switch (replyAddress)
	{
	case 0x1f:
		// short status: 00 ready, 11 not initialized, 44 searching the index
		aica::midiSend(state == Ready ? 0x00 : state == SearchingIndex ? 0x44 : 0x11);
		break;
	case 0x10:
		// long status: 00 initialized, 01 reset and waiting, 04 searching the index
		midiSend(base | 0x10, state == Ready ? 0 : state == SearchingIndex ? 4 : 1, 0);
		break;
	case 0x13:
		// wheel speed, centered
		midiSend(base | 0x13, 0x40, 0);
		break;
	case 0x14:
		{
			// encoder
			const int pos = wheelPosition();
			midiSend(base | 0x14, (pos >> 7) & 0x7f, pos & 0x7f);
		}
		break;
	case 0x1d:
		midiSend(base | 0x1d, paramSelect[0], paramSelect[1]);
		break;
	case 0x1e:
		{
			const u16 v = paramTable[(paramSelect[1] * 16 + paramSelect[0]) & 0xff];
			midiSend(base | 0x1e, (v >> 7) & 0x7f, v & 0x7f);
		}
		break;
	default:
		if (replyAddress < 0x10)
			// the parameters of the command 80 + address
			midiSend(base | replyAddress, params[replyAddress][0], params[replyAddress][1]);
		else
			midiSend(base | (replyAddress & 0x1f), 0, 0);
		break;
	}
}

static void process(u8 cmd, u8 b1, u8 b2)
{
	if (cmd < 0x90)
	{
		params[cmd & 0xf][0] = b1;
		params[cmd & 0xf][1] = b2;
	}
	switch (cmd)
	{
	case 0x80:
		// FFB mode. b1: 0 permanent, 1 short. b2: 1 enabled
		shortMode = b1 & 1;
		if (b2 == 0)
		{
			active = false;
			haptic::stopAll(0);
			if (calibrating) {
				calibrating = false;
				os_notify(i18n::T("Calibration done"), 2000);
			}
		}
		else
		{
			active = true;
			applyEffects();
		}
		break;

	case 0x81:
		// Maximum torque towards the center (b1) and away from it (b2)
		applyTorque();
		break;

	case 0x82:
		// Unknown "type", 7F 54 or 00 54. Only kept to be read back
		break;

	case 0x83:
		// Global gain, 40 at power-up, 20 in most games
		power = b1 / 127.f;
		applyEffects();
		break;

	case 0x84:
		// Constant torque: 01 00 none, 01 7F full left, 00 01 full right
		torque = ((b1 << 7) | b2) - 0x80;
		torqueDropped = false;
		applyTorque();
		break;

	case 0x85:
		applySine();
		break;

	case 0x86:
		// a visco speed of 0 is stored as 1
		if (b1 == 0)
			params[6][0] = 1;
		applyDamper();
		break;

	case 0x87:
	case 0x88:
	case 0x8b:
		applySpring();
		break;

	case 0x9d:
		paramSelect[0] = b1;
		paramSelect[1] = b2;
		break;

	case 0x9e:
		paramTable[(paramSelect[1] * 16 + paramSelect[0]) & 0xff] = (b1 << 7) | b2;
		break;

	case 0xf0:
		// The current position becomes the center, 0x2000
		centerOffset = position - 0x2000;
		break;

	case 0xfa:
		// What the board answers from now on
		replyAddress = b2;
		legacyReplies = false;
		break;

	case 0xfb:
		// [guess] plays a page of the effect table. Not emulated
		break;

	case 0xfc:
		// Phase alignment and index search: the board is busy for a while
		state = SearchingIndex;
		searchEnd = sh4_sched_now64() + IndexSearchTime;
		break;

	case 0xfd:
		// Status query
		break;

	case 0xff:
		// Reset
		resetBoard();
		active = false;
		haptic::stopAll(0);
		os_notify(i18n::T("Calibrating the wheel. Keep it centered."), 10000);
		calibrating = true;
		haptic::setSpring(0, 0.8f, 1.f);
		position = 8192.f;
		centerOffset = 0.f;
		break;

	default:
		break;
	}
}

static void midiReceiver(u8 data)
{
	// fake position used during calibration
	if (calibrating)
		position = std::clamp(position + torque, 0.f, 16383.f);
	if (data & 0x80)
		midiTxBufIndex = 0;
	midiTxBuf[midiTxBufIndex] = data;
	if (midiTxBufIndex == 3 && ((midiTxBuf[0] ^ midiTxBuf[1] ^ midiTxBuf[2]) & 0x7f) == midiTxBuf[3])
	{
		const u8 cmd = midiTxBuf[0];
		lastFrameTime = sh4_sched_now64();
		process(cmd, midiTxBuf[1], midiTxBuf[2]);

		if (!calibrating)
		{
			int direction = -1;
			if (NaomiGameInputs != nullptr)
				direction = NaomiGameInputs->axes[0].inverted ? 1 : -1;

			position = std::clamp(mapleInputState[0].fullAxes[0] * direction / 4.f + 8192.f, 0.f, 16383.f);
		}
		if (!passthrough)
			reply();

		// The status queries are only sent when a real board answers them
		if (cmd != 0xfd || passthrough)
			networkOutput.output("midiffb", (midiTxBuf[0] << 16) | (midiTxBuf[1]) << 8 | midiTxBuf[2]);
		if (cmd != 0xfd)
			DEBUG_LOG(NAOMI, "midiFFB: %02x %02x %02x", cmd, midiTxBuf[1], midiTxBuf[2]);
	}
	midiTxBufIndex = (midiTxBufIndex + 1) % std::size(midiTxBuf);
}

// Every ms: the end of the index search, the short mode's timeout, and the board's answers relayed by a network
// output client: "midiffb_rx = HH..", one answer per line, its bytes in hex. Only active once a client has sent one,
// and until that client disconnects.
static int pollCallback(int tag, int cycles, int jitter, void *arg)
{
	const u64 now = sh4_sched_now64();
	if (state == SearchingIndex && now >= searchEnd)
		state = Ready;
	if (active && shortMode && !torqueDropped && torque != 0 && now - lastFrameTime > ShortModeTimeout)
	{
		torqueDropped = true;
		haptic::setTorque(0, 0.f);
	}

	std::string answer;
	if (!networkOutput.hasInput("midiffb_rx"))
	{
		if (passthrough)
		{
			passthrough = false;
			NOTICE_LOG(NAOMI, "midiFFB: emulated answers");
		}
	}
	else if (!passthrough)
	{
		// The frames sent until now have been answered by the emulation
		passthrough = true;
		while (networkOutput.popInput("midiffb_rx", answer))
			;
		NOTICE_LOG(NAOMI, "midiFFB: the board's answers are relayed by the network output");
	}
	else
	{
		while (networkOutput.popInput("midiffb_rx", answer))
		{
			// "9440 0054" or "94 40 00 54" are accepted too
			answer.erase(std::remove_if(answer.begin(), answer.end(), [](char c) { return !std::isxdigit((u8)c); }), answer.end());
			for (size_t i = 0; i + 1 < answer.size(); i += 2)
				aica::midiSend((u8)strtoul(answer.substr(i, 2).c_str(), nullptr, 16));
		}
	}
	return PollPeriod;
}

void init()
{
	aica::setMidiReceiver(midiReceiver);
	initialized = true;
	if (pollSchedId == -1)
		pollSchedId = sh4_sched_register(0, pollCallback);
	reset();
}

void reset()
{
	resetBoard();
	active = false;
	calibrating = false;
	midiTxBufIndex = 0;
	position = 8192.f;
	centerOffset = 0.f;
	paramSelect[0] = paramSelect[1] = 0;
	paramTable.fill(0);
	// a hard reset cancels the scheduled callbacks
	if (pollSchedId != -1)
		sh4_sched_request(pollSchedId, PollPeriod);
}

void term()
{
	aica::setMidiReceiver(nullptr);
	initialized = false;
	passthrough = false;
	if (pollSchedId != -1)
	{
		sh4_sched_unregister(pollSchedId);
		pollSchedId = -1;
	}
}

void serialize(Serializer& ser)
{
	if (!initialized)
		return;
	ser << midiTxBuf;
	ser << midiTxBufIndex;
	ser << calibrating;
	ser << active;
	ser << shortMode;
	ser << power;
	ser << position;
	ser << centerOffset;
	ser << torque;
	ser << params;
	ser << replyAddress;
	ser << legacyReplies;
	ser << state;
	ser << paramSelect;
	ser << paramTable;
}

void deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V63)
	{
		if (initialized)
		{
			deser >> midiTxBuf;
			deser >> midiTxBufIndex;
			deser >> calibrating;
			deser >> active;
			deser >> shortMode;
			deser >> power;
			deser >> position;
			deser >> centerOffset;
			deser >> torque;
			deser >> params;
			deser >> replyAddress;
			deser >> legacyReplies;
			deser >> state;
			deser >> paramSelect;
			deser >> paramTable;
		}
	}
	else
	{
		// Older versions: the board's registers weren't saved
		if (initialized) {
			deser >> midiTxBuf;
			deser >> midiTxBufIndex;
		}
		else if (deser.version() < Deserializer::V51) {
			deser.skip(4);		// midiTxBuf
			deser.skip<u32>();	// midiTxBufIndex
		}
		if (initialized)
			deser >> calibrating;
		else if (deser.version() < Deserializer::V51)
			deser.skip<bool>();	// calibrating
		if (initialized)
		{
			resetBoard();
			state = Ready;
			replyAddress = 0x10;
			legacyReplies = true;
			centerOffset = 0.f;
			paramSelect[0] = paramSelect[1] = 0;
			paramTable.fill(0);
			if (deser.version() >= Deserializer::V51)
			{
				float damperParam, damperSpeed, oldTorque;
				deser >> active;
				deser >> power;
				deser >> damperParam;
				deser >> damperSpeed;
				deser >> position;
				deser >> oldTorque;
				torque = std::clamp((int)oldTorque, -128, 127);
				params[6][0] = (u8)std::clamp(damperSpeed * 127.f, 1.f, 127.f);
				params[6][1] = (u8)std::clamp(damperParam * 127.f, 0.f, 127.f);
				if (deser.version() >= Deserializer::V55)
				{
					u8 maxSpring;
					float springForce;
					deser >> maxSpring;
					deser >> springForce;
					params[0xb][0] = (u8)std::clamp(springForce * 127.f, 0.f, 127.f);
				}
			}
			else
			{
				active = false;
				position = 8192.f;
			}
		}
	}
	if (initialized)
	{
		lastFrameTime = sh4_sched_now64();
		torqueDropped = false;
		if (state == SearchingIndex)
			searchEnd = lastFrameTime + IndexSearchTime;
		if (active && !calibrating)
			applyEffects();
		else
			haptic::stopAll(0);
	}
	// not saved: polled again from now on
	if (pollSchedId != -1)
		sh4_sched_request(pollSchedId, PollPeriod);
}

}	// namespace midiffb

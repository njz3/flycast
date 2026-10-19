/*
	Copyright 2023 flyinghead

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
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef NAOMI_MULTIBOARD
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

struct SharedMemory;
struct MultiKeyboard;
class IpcMutex;

class Multiboard
{
public:
	Multiboard();
	~Multiboard();

	std::pair<u32, bool> readG1(u32 addr, u32 size);
	bool writeG1(u32 addr, u32 data, u32 size);

	u32 readG2Ext(u32 addr, u32 size);
	void writeG2Ext(u32 addr, u32 data, u32 size);

	bool dmaStart();
	void reset();
	void syncWait();
	static void keyboardEvent(u16 code, bool pressed);

private:
	bool isMaster() const { return boardId == 0; }
	bool isSlave() const { return boardId != 0; }
	void startSlave();
	bool waitForSync(std::unique_lock<IpcMutex>& lock, std::chrono::steady_clock::time_point deadline);
	static int schedCallback(int tag, int cycles, int jitter, void *arg);
	u16 bankStatus();

	int boardId = 0;
	u32 offset = 0;
	u32 reg9c = 0;
	SharedMemory *sharedMem;
#ifdef _WIN32
	HANDLE mapFile;
	std::string sharedMemId;
#else
	std::string sharedMemFileName;
#endif
	int boardCount = 0;
	bool slaveStarted = false;
	bool firstSyncDone = false;
	int schedId;
	int syncCycles;
	u16 g2status = 0;
	u16 dmaOffset = 0;
	std::unique_ptr<struct MultiKeyboard> keyboard;
	static Multiboard *Instance;

	// Trace of the shared memory and multiboard register accesses, one file per board, enabled
	// by naomi:MultiboardTrace. Written at each sync, so that the traces of a good and a bad
	// start can be compared slice by slice.
	void openTrace();
	void trace(const char *format, ...);
	void traceRead(const char *what, u32 addr, u32 value);
	void traceWrite(u32 addr, u32 value);
	void traceSync();
	FILE *traceFile = nullptr;
	std::string traceBuffer;
	u64 syncCount = 0;
	u32 sliceReads = 0;
	u32 sliceWrites = 0;
	u32 readHash = 0;
	u32 writeHash = 0;
	// Slave accesses to the shared memory while the live bank differs from the one seen at sync
	u32 sliceRaces = 0;
	u32 lastStatusWrite = ~0u;
	// Reads are only traced when the value changes
	std::unordered_map<u32, u32> lastReads;
};

#else // !NAOMI_MULTIBOARD

class Multiboard
{
public:
	std::pair<u32, bool> readG1(u32 addr, u32 size) {
		return std::make_pair(0, false);
	}
	bool writeG1(u32 addr, u32 data, u32 size) { return false; }

	u32 readG2Ext(u32 addr, u32 size) {
		return 0;
	}
	void writeG2Ext(u32 addr, u32 data, u32 size) { }

	bool dmaStart() {
		return false;
	}
	void reset() { }

	void syncWait() { }

	static void keyboardEvent(u16 code, bool pressed) { }
};

#endif // !NAOMI_MULTIBOARD

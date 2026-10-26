/*
	Copyright 2022 flyinghead

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
#include "output.h"
#include "oslib/i18n.h"
#include "stdclass.h"

NetworkOutput networkOutput;

void NetworkOutput::checkGame()
{
	if (catalogGame == settings.content.gameId)
		return;
	catalogGame = settings.content.gameId;
	catalog.clear();
	lastValues.clear();
	undescribed.clear();
}

void NetworkOutput::describe(const std::string& name, Type type, const std::string& meaning)
{
	std::lock_guard<std::mutex> _(catalogMutex);
	checkGame();
	for (Description& description : catalog)
		if (description.name == name)
		{
			description.type = type;
			description.meaning = meaning;
			return;
		}
	catalog.push_back({ name, type, meaning });
	undescribed.erase(std::remove(undescribed.begin(), undescribed.end(), name), undescribed.end());
}

void NetworkOutput::keep(const char *name, u32 value)
{
	std::lock_guard<std::mutex> _(catalogMutex);
	checkGame();
	auto it = lastValues.find(name);
	if (it == lastValues.end())
	{
		lastValues[name] = value;
		if (std::none_of(catalog.begin(), catalog.end(), [name](const Description& d) { return d.name == name; }))
			undescribed.push_back(name);
	}
	else {
		it->second = value;
	}
}

std::vector<NetworkOutput::Row> NetworkOutput::rows()
{
	std::lock_guard<std::mutex> _(catalogMutex);
	checkGame();
	std::vector<Row> rows;
	const bool active = server != INVALID_SOCKET;
	rows.push_back({ "game", i18n::Ts("text"), i18n::Ts("The game's name, the first line a client gets"),
		active ? settings.content.gameId : "-" });
	const auto value = [this, active](const std::string& name, Type type) -> std::string {
		auto it = lastValues.find(name);
		if (!active || it == lastValues.end())
			return "-";
		const u32 v = it->second;
		switch (type)
		{
		case Type::Bit:
			return std::to_string(v);
		case Type::Byte:
			return strprintf("%02X", v & 0xff);
		case Type::Midi:
			return strprintf("%02X %02X %02X", (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
		default:
			return strprintf("%X", v);
		}
	};
	for (const Description& d : catalog)
	{
		static const char * const types[] { "0 or 1", "byte, 00 to FF", "MIDI message, 3 bytes", "number" };
		rows.push_back({ d.name, i18n::Ts(types[(int)d.type]), d.meaning, value(d.name, d.type) });
	}
	for (const std::string& name : undescribed)
		rows.push_back({ name, "?", i18n::Ts("Not described"), strprintf("%X", lastValues[name]) });
	return rows;
}

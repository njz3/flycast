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
#pragma once
#include "types.h"
#include "net_platform.h"
#include "emulator.h"
#include "cfg/option.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// Outputs sent to TCP clients, one "name = value\n" line each, value in hex (same as mame).
// Bidirectional mode: the clients can send values back in the same format. A value is only used
// once a client has sent it, and is forgotten when that client disconnects. The clients are
// read without ever blocking, and sending never waits for them.
class NetworkOutput
{
	void init()
	{
		if (!config::NetworkOutput || settings.naomi.slave || settings.naomi.drivingSimSlave == 1) {
			term();
			return;
		}
		if (server != INVALID_SOCKET)
			// already done
			return;
		server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		int option = 1;
		setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));

		sockaddr_in saddr{};
		socklen_t saddr_len = sizeof(saddr);
		saddr.sin_family = AF_INET;
		saddr.sin_addr.s_addr = INADDR_ANY;
		saddr.sin_port = htons(8000 + settings.naomi.drivingSimSlave);
		if (::bind(server, (sockaddr *)&saddr, saddr_len) < 0)
		{
			perror("Network output: bind failed");
			term();
			return;
		}
		if (listen(server, 5) < 0)
		{
			perror("Network output: listen failed");
			term();
			return;
		}
		set_non_blocking(server);
		EventManager::listen(Event::VBlank, vblankCallback, this);
	}

public:
	void term()
	{
		EventManager::unlisten(Event::VBlank, vblankCallback, this);
		for (sock_t sock : clients)
			closesocket(sock);
		clients.clear();
		rxBuffers.clear();
		resetInputs();
		if (server != INVALID_SOCKET) {
			closesocket(server);
			server = INVALID_SOCKET;
		}
	}

	void reset() {
		init();
		gameNameSent = false;
	}

	void output(const char *name, u32 value)
	{
		if (clients.empty())
			return;
		if (!gameNameSent)
		{
			send("game = " + settings.content.gameId + "\n");
			gameNameSent = true;
		}
		char s[9];
		snprintf(s, sizeof(s), "%x", value);
		std::string msg = std::string(name) + " = " + std::string(s) + "\n";	// mame uses \r
		send(msg);
	}

	// True once a client has sent the given name back
	bool hasInput(const char *name)
	{
		poll();
		return latest.count(name) != 0;
	}

	// The last value received for name, in hex
	bool input(const char *name, u32& value)
	{
		poll();
		auto it = latest.find(name);
		if (it == latest.end())
			return false;
		value = (u32)strtoul(it->second.c_str(), nullptr, 16);
		return true;
	}

	// The values received for name, oldest first, each one once
	bool popInput(const char *name, std::string& value)
	{
		poll();
		auto it = queued.find(name);
		if (it == queued.end() || it->second.empty())
			return false;
		value = it->second.front();
		it->second.pop_front();
		return true;
	}

private:
	static void vblankCallback(Event event, void *param) {
		((NetworkOutput *)param)->acceptConnections();
		((NetworkOutput *)param)->poll();
	}

	// Read what the clients have sent, without blocking
	void poll()
	{
		if (clients.empty())
			return;
		std::vector<sock_t> closedSockets;
		for (sock_t sock : clients)
		{
			char buf[512];
			while (true)
			{
				int n = ::recv(sock, buf, sizeof(buf), 0);
				if (n > 0)
				{
					std::string& data = rxBuffers[sock];
					data.append(buf, n);
					parse(sock, data);
					continue;
				}
				if (n == 0) {
					// closed by the client
					closedSockets.push_back(sock);
				}
				else
				{
					int error = get_last_error();
					if (error != L_EWOULDBLOCK && error != L_EAGAIN)
						closedSockets.push_back(sock);
				}
				break;
			}
		}
		for (sock_t sock : closedSockets)
			removeClient(sock);
	}

	// Split the received data into "name = value" lines, ended by \n or \r
	void parse(sock_t sock, std::string& data)
	{
		size_t start = 0;
		for (size_t i = 0; i < data.size(); i++)
		{
			if (data[i] != '\n' && data[i] != '\r')
				continue;
			received(sock, data.substr(start, i - start));
			start = i + 1;
		}
		data.erase(0, start);
		if (data.size() > 1024)
			// not a line
			data.clear();
	}

	void received(sock_t sock, const std::string& line)
	{
		const auto trim = [](const std::string& s) {
			const size_t first = s.find_first_not_of(" \t");
			if (first == std::string::npos)
				return std::string();
			return s.substr(first, s.find_last_not_of(" \t") - first + 1);
		};
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			return;
		const std::string name = trim(line.substr(0, eq));
		const std::string value = trim(line.substr(eq + 1));
		if (name.empty() || value.empty())
			return;
		latest[name] = value;
		sources[name] = sock;
		std::deque<std::string>& values = queued[name];
		values.push_back(value);
		if (values.size() > 256)
			values.pop_front();
	}

	void removeClient(sock_t sock)
	{
		closesocket(sock);
		clients.erase(std::find(clients.begin(), clients.end(), sock));
		rxBuffers.erase(sock);
		// back to the emulated values for what this client sent
		for (auto it = sources.begin(); it != sources.end(); )
		{
			if (it->second == sock)
			{
				latest.erase(it->first);
				queued.erase(it->first);
				it = sources.erase(it);
			}
			else {
				++it;
			}
		}
	}

	void resetInputs()
	{
		latest.clear();
		queued.clear();
		sources.clear();
	}

	void acceptConnections()
	{
		sockaddr_in src_addr{};
		socklen_t addr_len = sizeof(src_addr);
		sock_t sockfd = accept(server, (sockaddr *)&src_addr, &addr_len);
		if (sockfd != INVALID_SOCKET)
		{
			set_non_blocking(sockfd);
			if (gameNameSent)
			{
				std::string msg = "game = " + settings.content.gameId + "\n";
				if (::send(sockfd, msg.c_str(), msg.length(), 0) < 0)
				{
					int error = get_last_error();
					if (error != L_EWOULDBLOCK && error != L_EAGAIN) {
						closesocket(sockfd);
						return;
					}
				}
			}
			clients.push_back(sockfd);
		}
	}

	void send(const std::string& msg)
	{
		std::vector<sock_t> errorSockets;
		for (sock_t sock : clients)
			if (::send(sock, msg.c_str(), msg.length(), 0) < 0)
			{
				int error = get_last_error();
				if (error != L_EWOULDBLOCK && error != L_EAGAIN)
					errorSockets.push_back(sock);
			}
		for (sock_t sock : errorSockets)
			removeClient(sock);
	}

	sock_t server = INVALID_SOCKET;
	std::vector<sock_t> clients;
	bool gameNameSent = false;
	// Bidirectional mode: what the clients send back
	std::unordered_map<sock_t, std::string> rxBuffers;
	std::unordered_map<std::string, std::string> latest;
	std::unordered_map<std::string, std::deque<std::string>> queued;
	// the client that sent each name
	std::unordered_map<std::string, sock_t> sources;
};

extern NetworkOutput networkOutput;


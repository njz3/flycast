/*
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
#include "input/gamepad_device.h"
#include "input/mouse.h"
#include "sdl.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

class SDLGamepad : public GamepadDevice
{
public:
	SDLGamepad(int maple_port, int joystick_idx, SDL_Joystick *sdl_joystick);
	~SDLGamepad() override;
	void set_maple_port(int port) override;

	void rumble(float power, float inclination, u32 duration_ms) override;
	void update_rumble() override;

	void setSine(float power, float freq, u32 duration_ms);
	void setTorque(float torque);
	void setSpring(float saturation, float speed, float center = 0.f);
	void setDamper(float param, float speed);
	void stopHaptic();

	virtual void close();

	const char *get_button_name(u32 code) override;
	const char *get_axis_name(u32 code) override;

	std::shared_ptr<InputMapping> getDefaultMapping() override;
	void resetMappingToDefault(bool arcade, bool gamepad) override;
	bool find_mapping(int system = settings.platform.system) override;

	static void AddSDLGamepad(std::shared_ptr<SDLGamepad> gamepad)
	{
		sdl_gamepads[gamepad->sdl_joystick_instance] = gamepad;
		GamepadDevice::Register(gamepad);
	}
	static std::shared_ptr<SDLGamepad> GetSDLGamepad(SDL_JoystickID id)
	{
		auto it = sdl_gamepads.find(id);
		if (it != sdl_gamepads.end())
			return it->second;
		else
			return NULL;
	}
	static void closeAllGamepads()
	{
		while (!sdl_gamepads.empty())
			sdl_gamepads.begin()->second->close();
	}
	static void UpdateRumble() {
		for (auto &[k, gamepad] : sdl_gamepads)
			gamepad->update_rumble();
	}

	static void SetTorque(int port, float torque) {
		applyToPort(port, &SDLGamepad::setTorque, torque);
	}
	static void SetSpring(int port, float saturation, float speed, float center = 0.f) {
		applyToPort(port, &SDLGamepad::setSpring, saturation, speed, center);
	}
	static void SetDamper(int port, float param, float speed) {
		applyToPort(port, &SDLGamepad::setDamper, param, speed);
	}
	static void SetSine(int port, float power, float freq, u32 duration_ms) {
		applyToPort(port, &SDLGamepad::setSine, power, freq, duration_ms);
	}
	static void StopHaptic(int port) {
		applyToPort(port, &SDLGamepad::stopHaptic);
	}
	// The game ended: the force feedback is stopped before returning since the process may end right after,
	// and the effects requested until the next game starts are ignored
	static void EndGameHaptic() {
		for (auto &[k, gamepad] : sdl_gamepads)
			gamepad->endGameHaptic();
	}
	static void StartGameHaptic() {
		for (auto &[k, gamepad] : sdl_gamepads)
			gamepad->startGameHaptic();
	}

	// Player 1's force feedback goes to the device the ForceFeedbackDevice option names, whatever port its axes are
	// read for: the first connected one with force feedback whose name is the same, case and surrounding spaces
	// aside. -1: AUTO, or that device isn't connected, so the device on player 1's port
	static SDL_JoystickID ChosenForceFeedbackDevice();
	static SDL_JoystickID GetForceFeedbackDevice() {
		return ffbDevice;
	}
	static void SetForceFeedbackDevice(SDL_JoystickID device) {
		ffbDevice = device;
	}
	// The connected devices with force feedback, each name once
	static std::vector<std::string> ForceFeedbackDeviceNames();
	static bool SameDeviceName(const std::string& a, const std::string& b);

protected:
	u64 vib_stop_time = 0;
	SDL_JoystickID sdl_joystick_instance;

private:
	template<typename Func, typename... Args>
	static void applyToPort(int port, Func func, Args&&... args)
	{
		const SDL_JoystickID device = port == 0 ? ffbDevice.load() : -1;
		for (auto &[k, gamepad] : sdl_gamepads)
			if (device != -1 ? gamepad->sdl_joystick_instance == device : gamepad->maple_port() == port)
				((*gamepad).*func)(std::forward<Args>(args)...);
	}
	// Written by the UI thread, read by the emulator's
	static std::atomic<SDL_JoystickID> ffbDevice;
	u16 getRumbleIntensity(float power) const;
	void doRumble(float power, u32 duration_ms);

	// The force feedback is sent to the device by a thread of its own: each haptic call is one or more
	// synchronous USB transfers, which can take several ms with a real wheel and would slow down the
	// emulation. Only the last request of each effect is kept until the thread applies it.
	enum HapticRequest { HapticStopAll, HapticTorque, HapticSpring, HapticDamper, HapticSine };
	struct EffectState
	{
		SDL_HapticEffect effect;	// as last sent to the device
		bool playing = false;		// started and not stopped since
		bool infinite = false;		// and playing until stopped
	};
	void startHapticThread();
	void stopHapticThread();
	void postHaptic(HapticRequest request, std::function<void()>&& func);
	void endGameHaptic();
	void startGameHaptic();
	// Run by the haptic thread
	void applySine(float power, float freq, u32 duration_ms);
	void applyTorque(float torque);
	void applySpring(float saturation, float speed, float center);
	void applyDamper(float param, float speed);
	void applyStopAll();
	void playEffect(int effectId, SDL_HapticEffect& effect, EffectState& state, bool infinite = true);
	void stopEffect(int effectId, EffectState& state);

	SDL_Joystick* sdl_joystick;
	float vib_inclination = 0;
	SDL_GameController *sdl_controller = nullptr;
	static std::map<SDL_JoystickID, std::shared_ptr<SDLGamepad>> sdl_gamepads;
	SDL_Haptic *haptic = nullptr;
	bool hapticRumble = false;
	bool hasAutocenter = false;
	bool isWheel = false;
	int sineEffectId = -1;
	int constEffectId = -1;
	int springEffectId = -1;
	int damperEffectId = -1;

	std::thread hapticThread;
	std::mutex hapticMutex;
	std::condition_variable hapticCond;
	std::map<HapticRequest, std::function<void()>> hapticRequests;
	bool hapticThreadRunning = false;
	// No effect between the end of a game and the start of the next one
	bool hapticBlocked = false;
	// To wait until a stop has been sent to the device
	std::condition_variable hapticStopCond;
	u64 hapticStopsRequested = 0;
	u64 hapticStopsDone = 0;
	// Only used by the haptic thread
	EffectState sineState {};
	EffectState constState {};
	EffectState springState {};
	EffectState damperState {};
	int autocenter = -1;
};

class SDLMouse : public Mouse
{
public:
	SDLMouse(u32 mouseId);
	void setAbsPos(int x, int y);
	void refreshName() override;

private:
	u32 mouseId;
};

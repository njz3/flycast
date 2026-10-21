#include "types.h"
#include "stdclass.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "serialize.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"

#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#ifdef _WIN32
#include <algorithm>
#elif defined(__ANDROID__)
#include <unistd.h>
#endif

static std::string user_config_dir;
static std::string user_data_dir;
static std::vector<std::string> system_config_dirs;
static std::vector<std::string> system_data_dirs;
static std::string write_dir;
static std::string original_data_dir;

const std::string defaultWs(" \0", 2);

bool file_exists(const std::string& filename)
{
	return (flycast::access(filename.c_str(), R_OK) == 0);
}

static void appendPathSep(std::string& str)
{
#ifdef _WIN32
	if (str.empty() || (str.back() != '/' && str.back() != '\\'))
		str += '\\';
#else
	if (str.empty() || str.back() != '/')
		str += '/';
#endif
}

void set_user_config_dir(const std::string& dir)
{
	user_config_dir = dir;
	appendPathSep(user_config_dir);
}

void set_user_data_dir(const std::string& dir)
{
	user_data_dir = dir;
	appendPathSep(user_data_dir);
}

void add_system_config_dir(const std::string& dir)
{
	std::string s(dir);
	appendPathSep(s);
	system_config_dirs.push_back(s);
}

void add_system_data_dir(const std::string& dir)
{
	std::string s(dir);
	appendPathSep(s);
	system_data_dirs.push_back(s);
}

std::string get_writable_config_path(const std::string& filename)
{
	/* Only stuff in the user_config_dir is supposed to be writable,
	 * so we always return that.
	 */
	return user_config_dir + filename;
}

std::string get_readonly_config_path(const std::string& filename)
{
	std::string user_filepath = get_writable_config_path(filename);
	if (file_exists(user_filepath))
		return user_filepath;

	for (const auto& config_dir : system_config_dirs)
	{
		std::string filepath = config_dir + filename;
		if (file_exists(filepath))
			return filepath;
	}

	// Not found, so we return the user variant
	return user_filepath;
}

std::string get_writable_data_path(const std::string& filename)
{
	/* Only stuff in the user_data_dir is supposed to be writable,
	 * so we always return that.
	 */
	return user_data_dir + filename;
}

std::string get_readonly_data_path(const std::string& filename)
{
	std::string user_filepath = get_writable_data_path(filename);
	if (file_exists(user_filepath))
		return user_filepath;

	for (const auto& data_dir : system_data_dirs)
	{
		std::string filepath = data_dir + filename;
		if (file_exists(filepath))
			return filepath;
	}
	// Try the game directory
	try {
		std::string parent = hostfs::storage().getParentPath(settings.content.path);
		std::string filepath = hostfs::storage().getSubPath(parent, filename);
		if (hostfs::storage().exists(filepath))
			return filepath;
	} catch (const FlycastException&) { }

	// Not found, so we return the user variant
	return user_filepath;
}

size_t get_last_slash_pos(const std::string& path)
{
	size_t lastindex = path.find_last_of('/');
#ifdef _WIN32
	size_t lastindex2 = path.find_last_of('\\');
	if (lastindex == std::string::npos)
		lastindex = lastindex2;
	else if (lastindex2 != std::string::npos)
		lastindex = std::max(lastindex, lastindex2);
#endif
	return lastindex;
}

std::string get_game_save_prefix()
{
	return get_game_save_dir() + settings.content.fileName;
}

void set_write_dir(const std::string& dir, const std::string& originalDataDir)
{
	write_dir = dir;
	appendPathSep(write_dir);
	original_data_dir = originalDataDir;
	appendPathSep(original_data_dir);
}

const std::string& get_write_dir() {
	return write_dir;
}

static bool copyFile(const std::string& from, const std::string& to)
{
	hostfs::File *in = hostfs::storage().openFile(from, "rb");
	if (in == nullptr)
		return false;
	hostfs::File *out = hostfs::storage().openFile(to, "wb");
	bool success = out != nullptr;
	if (success)
	{
		u8 buf[4096];
		size_t n;
		while (success && (n = in->read(buf, 1, sizeof(buf))) > 0)
			success = out->write(buf, 1, n) == n;
		delete out;
	}
	delete in;
	return success;
}

// The saves of the current game: the files named after the rom (nvmem, eeprom, cards...), its savestates and the VMUs
static bool isGameSave(const std::string& name)
{
	const std::string& fileName = settings.content.fileName;
	if (name.compare(0, fileName.size(), fileName) == 0)
		return true;
	const std::string base = get_file_basename(fileName);
	if (name.size() > base.size() && name.compare(0, base.size(), base) == 0
			&& (name[base.size()] == '.' || name[base.size()] == '_') && get_file_extension(name) == "state")
		return true;
	return name.compare(0, 9, "vmu_save_") == 0;
}

static void copyOriginalSaves(const std::string& dir)
{
	DIR *d = flycast::opendir(original_data_dir.c_str());
	if (d == nullptr)
		return;
	while (dirent *entry = flycast::readdir(d))
	{
		std::string name = entry->d_name;
		if (!isGameSave(name))
			continue;
		if (copyFile(original_data_dir + name, dir + name))
			INFO_LOG(COMMON, "Save %s copied from %s", name.c_str(), original_data_dir.c_str());
		else
			WARN_LOG(COMMON, "Can't copy the save %s from %s to %s", name.c_str(), original_data_dir.c_str(), dir.c_str());
	}
	flycast::closedir(d);
}

std::string get_game_save_dir()
{
	if (write_dir.empty() || settings.content.fileName.empty())
		return user_data_dir;
	// Named after the rom set: the file name without its extension
	std::string game = get_file_basename(settings.content.fileName);
	for (char& c : game)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
			c = '_';
	std::string dir = write_dir + game;
	const bool created = !file_exists(dir) && make_directory(dir);
	appendPathSep(dir);
	if (created)
		copyOriginalSaves(dir);
	return dir;
}

bool make_directory(const std::string& path)
{
	return flycast::mkdir(path.c_str(), 0755) == 0;
}

void cThread::Start()
{
	verify(!thread.joinable());
	thread = std::thread([this]() {
		ThreadName _(name);
		entry(param);
	});
}

void cThread::WaitToEnd()
{
	if (thread.joinable() && thread.get_id() != std::this_thread::get_id())
		thread.join();
}

cResetEvent::cResetEvent() : state(false)
{

}

cResetEvent::~cResetEvent() = default;

void cResetEvent::Set()//Signal
{
    std::lock_guard<std::mutex> lock(mutx);

    state = true;
    cond.notify_one();
}

void cResetEvent::Reset()
{
    std::lock_guard<std::mutex> lock(mutx);

    state = false;
}

bool cResetEvent::Wait(u32 msec)
{
	std::unique_lock<std::mutex> lock(mutx);

	if (!state)
		cond.wait_for(lock, std::chrono::milliseconds(msec));

	bool rc = state;
    state = false;

    return rc;
}

void cResetEvent::Wait()
{
    std::unique_lock<std::mutex> lock(mutx);

    if (!state) {
        cond.wait(lock);
    }

    state = false;
}

#if defined(__ANDROID__) && (HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_X64)
const unsigned long PAGE_SIZE = (unsigned long)sysconf(_SC_PAGESIZE);
#endif

void RamRegion::serialize(Serializer &ser) const {
	ser.serialize(data, size);
}

void RamRegion::deserialize(Deserializer &deser) {
	deser.deserialize(data, size);
}


void RamRegion::alloc(size_t size)
{
	this->size = size;
	data = (u8 *)allocAligned(PAGE_SIZE, size);
	ownsMemory = true;
}

void RamRegion::free()
{
	this->size = 0;
	if (ownsMemory)
		freeAligned(data);
	data = nullptr;
}

u64 getTimeMs()
{
	using the_clock = std::chrono::steady_clock;
	std::chrono::time_point<the_clock> now = the_clock::now();
	static std::chrono::time_point<the_clock> start = now;

	return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
}

#ifdef _WIN32
static struct tm *localtime_r(const time_t *_clock, struct tm *_result)
{
	return localtime_s(_result, _clock) ? nullptr : _result;
}
#endif

std::string timeToISO8601(time_t time)
{
	tm t;
	if (localtime_r(&time, &t) == nullptr)
		return {};
	return strprintf("%04d/%02d/%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

std::string timeToShortDateTimeString(time_t time) {
	return i18n::formatShortDateTime(time);
}


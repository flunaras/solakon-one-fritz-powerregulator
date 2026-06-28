#include "statestore.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

namespace {

// Splits `path` into its parent directory (everything before the last '/'),
// or "" if `path` contains no '/'.
std::string parentDir(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// Recursively creates `dir` (like `mkdir -p`), best-effort. Returns true if
// the directory exists (or was created) afterwards, false otherwise.
bool mkdirsRecursive(const std::string& dir) {
    if (dir.empty() || dir == "/") return true;

    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);

    const std::string parent = parentDir(dir);
    if (!parent.empty() && !mkdirsRecursive(parent))
        return false;

    if (::mkdir(dir.c_str(), 0755) == 0)
        return true;
    // Another process/thread may have created it concurrently.
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

bool loadState(const std::string& path, PersistedState& out) {
    if (path.empty())
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;

    std::ostringstream ss;
    ss << in.rdbuf();

    json j;
    try {
        j = json::parse(ss.str());
    } catch (const json::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    PersistedState parsed;
    if (j.contains("low_soc_hold") && j["low_soc_hold"].is_boolean())
        parsed.low_soc_hold = j["low_soc_hold"].get<bool>();
    else
        return false;

    out = parsed;
    return true;
}

bool saveState(const std::string& path, const PersistedState& state) {
    if (path.empty())
        return false;

    const std::string dir = parentDir(path);
    if (!dir.empty() && !mkdirsRecursive(dir))
        return false;

    json j;
    j["low_soc_hold"] = state.low_soc_hold;

    // Write to a temporary file in the same directory, then rename it into
    // place -- rename() is atomic on the same filesystem, so a crash or
    // power loss mid-write leaves either the old file or the new one fully
    // intact, never a truncated/corrupt one.
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << j.dump(2) << '\n';
        if (!out.good())
            return false;
    }

    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

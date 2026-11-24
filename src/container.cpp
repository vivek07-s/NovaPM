#include "process_collector.hpp"
#include <cctype>

// Finds a 64-character hex container id inside a cgroup path and names the runtime.
std::string containerFromCgroup(const std::string& t) {
    auto isHex = [](char c) { return std::isxdigit((unsigned char)c) != 0; };
    std::string id;
    for (size_t i = 0; i + 64 <= t.size() && id.empty(); ++i) {
        if (i > 0 && isHex(t[i - 1])) continue;                 // must start a hex run
        size_t j = i;
        while (j < t.size() && isHex(t[j])) ++j;
        if (j - i == 64) id = t.substr(i, 12);
        i = j;
    }
    if (id.empty()) return "";
    auto has = [&](const char* k) { return t.find(k) != std::string::npos; };
    const char* rt = has("docker") ? "docker" : has("crio") ? "crio" : has("libpod") ? "podman"
                   : has("containerd") ? "containerd" : "container";
    return std::string(rt) + ":" + id;
}

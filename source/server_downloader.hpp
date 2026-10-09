#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <wups/config_api.h>

namespace ServerDownloader {

inline constexpr const char *kBuildVersion = "1.0.2";

struct DownloadFile {
    std::string name;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};

struct ServerInfo {
    std::string name;
    std::string description;
    std::string version;
    std::vector<DownloadFile> files;
    std::string releaseNotes;
    std::string compatibility;
};


void AddDownloadServersMenu(WUPSConfigCategoryHandle rootHandle);

void AddVersionStatusMenu(WUPSConfigCategoryHandle rootHandle);

bool Initialize();

bool DownloadServer(const ServerInfo& server);

}

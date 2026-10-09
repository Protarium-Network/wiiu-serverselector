#include "client_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <coreinit/debug.h>

namespace ClientManager {
namespace {

constexpr const char *kClientsRoot = "/vol/external01/wiiu/wiiu-clients";
constexpr const char *kModulesRoot = "/vol/external01/wiiu/environments/aroma/modules";
constexpr const char *kPluginsRoot = "/vol/external01/wiiu/environments/aroma/plugins";

bool IsInternalFolder(const std::string &name) {
    return name == ".downloads" || name.rfind(".serverselector-download-", 0) == 0;
}

bool IsClientFile(const std::string &name) {
    return name.size() > 4 &&
           (name.compare(name.size() - 4, 4, ".wms") == 0 ||
            name.compare(name.size() - 4, 4, ".wps") == 0);
}

std::string InstalledPath(const std::string &name) {
    const bool isModule = name.compare(name.size() - 4, 4, ".wms") == 0;
    return std::string(isModule ? kModulesRoot : kPluginsRoot) + "/" + name;
}

bool FilesEqual(const std::string &left, const std::string &right) {
    struct stat leftStat{}, rightStat{};
    if (stat(left.c_str(), &leftStat) != 0 || stat(right.c_str(), &rightStat) != 0 ||
        leftStat.st_size != rightStat.st_size) return false;

    FILE *leftFile = std::fopen(left.c_str(), "rb");
    FILE *rightFile = std::fopen(right.c_str(), "rb");
    if (!leftFile || !rightFile) {
        if (leftFile) std::fclose(leftFile);
        if (rightFile) std::fclose(rightFile);
        return false;
    }

    std::vector<unsigned char> leftBuffer(16 * 1024), rightBuffer(16 * 1024);
    bool equal = true;
    while (true) {
        const size_t leftCount = std::fread(leftBuffer.data(), 1, leftBuffer.size(), leftFile);
        const size_t rightCount = std::fread(rightBuffer.data(), 1, rightBuffer.size(), rightFile);
        if (leftCount != rightCount || !std::equal(leftBuffer.begin(), leftBuffer.begin() + leftCount,
                                                    rightBuffer.begin())) {
            equal = false;
            break;
        }
        if (leftCount < leftBuffer.size()) break;
    }

    std::fclose(leftFile);
    std::fclose(rightFile);
    return equal;
}

bool IsActive(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (!directory) return false;

    bool foundFile = false;
    bool active = true;
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !IsClientFile(name)) continue;
        foundFile = true;
        if (!FilesEqual(path + "/" + name, InstalledPath(name))) {
            active = false;
            break;
        }
    }
    closedir(directory);
    return foundFile && active;
}

bool CopyFile(const std::string &source, const std::string &destination) {
    FILE *input = std::fopen(source.c_str(), "rb");
    FILE *output = std::fopen(destination.c_str(), "wb");
    if (!input || !output) {
        if (input) std::fclose(input);
        if (output) std::fclose(output);
        return false;
    }

    std::vector<unsigned char> buffer(16 * 1024);
    bool success = true;
    while (true) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count > 0 && std::fwrite(buffer.data(), 1, count, output) != count) {
            success = false;
            break;
        }
        if (count < buffer.size()) {
            success = !std::ferror(input);
            break;
        }
    }
    std::fflush(output);
    std::fclose(input);
    std::fclose(output);
    if (!success) unlink(destination.c_str());
    return success;
}

void RemoveInstalledClients(const std::vector<Client> &clients) {
    for (const Client &client : clients) {
        DIR *directory = opendir(client.path.c_str());
        if (!directory) continue;
        while (dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != ".." && IsClientFile(name)) unlink(InstalledPath(name).c_str());
        }
        closedir(directory);
    }
}

} // namespace

std::vector<Client> Scan() {
    std::vector<Client> clients;
    DIR *directory = opendir(kClientsRoot);
    if (!directory) return clients;

    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || IsInternalFolder(name)) continue;
        const std::string path = std::string(kClientsRoot) + "/" + name;
        struct stat info{};
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            clients.push_back({name, path, "client_" + name, IsActive(path)});
        }
    }
    closedir(directory);
    std::sort(clients.begin(), clients.end(), [](const Client &a, const Client &b) { return a.name < b.name; });
    return clients;
}

bool Install(const std::string &clientPath) {
    const std::vector<Client> clients = Scan();
    RemoveInstalledClients(clients);

    DIR *directory = opendir(clientPath.c_str());
    if (!directory) return false;

    bool success = true;
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !IsClientFile(name)) continue;
        if (!CopyFile(clientPath + "/" + name, InstalledPath(name))) {
            OSReport("[ServerSelector] failed to install %s\n", name.c_str());
            success = false;
            break;
        }
    }
    closedir(directory);
    return success;
}

} // namespace ClientManager
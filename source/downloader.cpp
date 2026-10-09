#include "server_downloader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <zlib.h>

#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <wups/config/WUPSConfigItemStub.h>

namespace ServerDownloader {
namespace {

constexpr const char *kClientsRoot = "/vol/external01/wiiu/wiiu-clients";
constexpr const char *kReleaseApiBase = "https://api.github.com/repos/";
constexpr const char *kPluginReleaseUrl = "https://api.github.com/repos/Protarium-Network/wiiu-serverselector/releases/latest";

struct ReleaseSource {
    const char *repository;
    const char *displayName;
    bool useArchive;
};

constexpr ReleaseSource kReleaseSources[] = {
    {"PretendoNetwork/Inkay", "Pretendo Inkay Official", true},
    {"Protarium-Network/Inkay-GitHub-Release", "Protarium Inkay", false},
};

struct DownloadItem {
    WUPSConfigItemHandle handle{};
    char *identifier = nullptr;
    ServerInfo server;
};

struct VersionItem {
    WUPSConfigItemHandle handle{};
    char *identifier = nullptr;
};

struct Transfer {
    FILE *file = nullptr;
    int percent = 0;
};

std::vector<ServerInfo> gServers;
std::vector<std::string> gReleaseErrors;
std::vector<DownloadItem *> gItems;
ServerInfo gPendingServer;
OSThread gWorkerThread;
alignas(16) uint8_t gWorkerStack[64 * 1024];
volatile bool gBusy = false;
volatile int gProgress = 0;
char gStatus[128] = "Ready";
char gVersionStatus[320] = "Check version";
bool gCurlReady = false;

void SetStatus(const char *value) {
    std::snprintf(gStatus, sizeof(gStatus), "%s", value ? value : "");
}

void SetVersionStatus(const std::string &value) {
    std::snprintf(gVersionStatus, sizeof(gVersionStatus), "%s", value.c_str());
}

bool IsHttps(const std::string &url) { return url.rfind("https://", 0) == 0; }

bool IsSafeName(const std::string &value) {
    if (value.empty() || value == "." || value == ".." || value.find("..") != std::string::npos) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ' ';
    });
}

bool IsSha256(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

struct ParsedVersion {
    unsigned components[3]{};
    bool prerelease = false;
};

bool ParseVersion(const std::string &version, ParsedVersion &parsed) {
    size_t position = 0;
    size_t component = 0;
    while (position < version.size() && component < 3) {
        if (!std::isdigit(static_cast<unsigned char>(version[position]))) {
            ++position;
            continue;
        }

        unsigned value = 0;
        while (position < version.size() && std::isdigit(static_cast<unsigned char>(version[position]))) {
            const unsigned digit = static_cast<unsigned>(version[position] - '0');
            if (value > 999999 || value * 10 + digit > 999999) return false;
            value = value * 10 + digit;
            ++position;
        }
        parsed.components[component++] = value;
    }
    if (component != 3) return false;

    std::string normalized = version;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    parsed.prerelease = normalized.find("alpha") != std::string::npos ||
                        normalized.find("beta") != std::string::npos ||
                        normalized.find("rc") != std::string::npos ||
                        normalized.find("preview") != std::string::npos;
    return true;
}

int CompareVersions(const ParsedVersion &left, const ParsedVersion &right) {
    for (size_t index = 0; index < 3; ++index) {
        if (left.components[index] != right.components[index]) {
            return left.components[index] > right.components[index] ? 1 : -1;
        }
    }
    if (left.prerelease == right.prerelease) return 0;
    return left.prerelease ? -1 : 1;
}

bool Request(const std::string &url, std::string &result);

void AppendUtf8(std::string &value, uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        value.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        value.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        value.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        value.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        value.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        value.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        value.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        value.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        value.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        value.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

bool ReadHexCodeUnit(const std::string &json, size_t &position, uint16_t &codeUnit) {
    if (position + 4 > json.size()) return false;
    codeUnit = 0;
    for (size_t index = 0; index < 4; ++index) {
        const unsigned char character = static_cast<unsigned char>(json[position++]);
        if (!std::isxdigit(character)) return false;
        codeUnit = static_cast<uint16_t>((codeUnit << 4) |
                    (std::isdigit(character) ? character - '0' : std::tolower(character) - 'a' + 10));
    }
    return true;
}

bool ReadUnicodeCodePoint(const std::string &json, size_t &position, uint32_t &codePoint) {
    uint16_t first = 0;
    if (!ReadHexCodeUnit(json, position, first)) return false;
    if (first >= 0xd800 && first <= 0xdbff) {
        if (position + 2 > json.size() || json[position] != '\\' || json[position + 1] != 'u') return false;
        position += 2;
        uint16_t second = 0;
        if (!ReadHexCodeUnit(json, position, second) || second < 0xdc00 || second > 0xdfff) return false;
        codePoint = 0x10000 + ((static_cast<uint32_t>(first) - 0xd800) << 10) +
                    (static_cast<uint32_t>(second) - 0xdc00);
        return true;
    }
    if (first >= 0xdc00 && first <= 0xdfff) return false;
    codePoint = first;
    return true;
}

bool ExtractJsonString(const std::string &json, size_t start, const char *key, std::string &value) {
    const size_t keyPosition = json.find(std::string("\"") + key + "\"", start);
    if (keyPosition == std::string::npos) return false;
    size_t valueStart = keyPosition + std::strlen(key) + 2;
    valueStart = json.find(':', valueStart);
    if (valueStart == std::string::npos) return false;
    ++valueStart;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart]))) ++valueStart;
    if (valueStart >= json.size() || json[valueStart] != '"') return false;
    ++valueStart;
    value.clear();
    while (valueStart < json.size()) {
        const char current = json[valueStart++];
        if (current == '"') return true;
        if (current != '\\') {
            value.push_back(current);
            continue;
        }
        if (valueStart >= json.size()) return false;
        const char escaped = json[valueStart++];
        switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                uint32_t codePoint = 0;
                if (!ReadUnicodeCodePoint(json, valueStart, codePoint)) return false;
                AppendUtf8(value, codePoint);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool ExtractJsonObjects(const std::string &json, const char *key, std::vector<std::string> &objects) {
    const size_t keyPosition = json.find(std::string("\"") + key + "\"");
    if (keyPosition == std::string::npos) return false;
    const size_t arrayStart = json.find('[', keyPosition);
    if (arrayStart == std::string::npos) return false;

    size_t position = arrayStart + 1;
    while (position < json.size()) {
        while (position < json.size() &&
               (std::isspace(static_cast<unsigned char>(json[position])) || json[position] == ',')) ++position;
        if (position >= json.size()) return false;
        if (json[position] == ']') return true;
        if (json[position] != '{') return false;

        const size_t objectStart = position;
        size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; position < json.size(); ++position) {
            const char current = json[position];
            if (inString) {
                if (escaped) escaped = false;
                else if (current == '\\') escaped = true;
                else if (current == '"') inString = false;
                continue;
            }
            if (current == '"') inString = true;
            else if (current == '{') ++depth;
            else if (current == '}' && --depth == 0) {
                objects.push_back(json.substr(objectStart, position - objectStart + 1));
                ++position;
                break;
            }
        }
        if (depth != 0) return false;
    }
    return false;
}

bool HasExtension(const std::string &name, const char *extension) {
    const size_t extensionLength = std::strlen(extension);
    return name.size() > extensionLength &&
           name.compare(name.size() - extensionLength, extensionLength, extension) == 0;
}

std::string CleanReleaseLine(const std::string &line) {
    std::string cleaned;
    bool pendingSpace = false;
    for (unsigned char character : line) {
        if (character == '*' || character == '`' || character == '#') continue;
        if (character < 0x20 || std::isspace(character)) {
            pendingSpace = !cleaned.empty();
            continue;
        }
        if (pendingSpace) cleaned.push_back(' ');
        cleaned.push_back(static_cast<char>(character));
        pendingSpace = false;
    }
    while (cleaned.rfind("- ", 0) == 0 || cleaned.rfind("+ ", 0) == 0) cleaned.erase(0, 2);
    return cleaned;
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void ExtractReleaseDetails(const std::string &json, std::string &releaseNotes, std::string &compatibility) {
    std::string body;
    if (!ExtractJsonString(json, 0, "body", body)) {
        releaseNotes = "No release notes published";
        compatibility = "No compatibility requirement stated";
        return;
    }

    size_t lineStart = 0;
    while (lineStart < body.size()) {
        const size_t lineEnd = body.find('\n', lineStart);
        const std::string rawLine = body.substr(lineStart,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
        const std::string line = CleanReleaseLine(rawLine);
        const std::string lowerLine = Lowercase(line);
        if (compatibility.empty() && lowerLine.find("aroma") != std::string::npos) {
            size_t requirementStart = lowerLine.find("requires");
            if (requirementStart == std::string::npos) requirementStart = lowerLine.find("required");
            if (requirementStart != std::string::npos) {
                compatibility = line.substr(requirementStart);
                if (!compatibility.empty()) compatibility[0] = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(compatibility[0])));
            }
        }

        const bool heading = !rawLine.empty() && rawLine.find_first_not_of(" \t") != std::string::npos &&
                             rawLine[rawLine.find_first_not_of(" \t")] == '#';
        const bool compatibilityLine = lowerLine.find("aroma") != std::string::npos &&
                                       (lowerLine.find("require") != std::string::npos);
        const bool changelogLink = lowerLine.find("full changelog") != std::string::npos ||
                                   lowerLine.find("http://") != std::string::npos ||
                                   lowerLine.find("https://") != std::string::npos;
        if (!line.empty() && !heading && !compatibilityLine && !changelogLink && releaseNotes.size() < 96) {
            if (!releaseNotes.empty()) releaseNotes += "; ";
            const size_t remaining = 112 - releaseNotes.size();
            releaseNotes += line.substr(0, remaining);
        }

        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }

    if (releaseNotes.empty()) releaseNotes = "No release notes published";
    if (compatibility.empty()) compatibility = "No compatibility requirement stated";
}

bool ParseReleaseAssets(const std::string &json, const ReleaseSource &source, ServerInfo &server) {
    std::string version;
    if (!ExtractJsonString(json, 0, "tag_name", version) || version.empty()) return false;
    server = {source.displayName, source.repository, version, {}, {}, {}};
    ExtractReleaseDetails(json, server.releaseNotes, server.compatibility);

    std::vector<std::string> assets;
    if (!ExtractJsonObjects(json, "assets", assets)) return false;
    for (const std::string &asset : assets) {
        std::string assetName;
        if (!ExtractJsonString(asset, 0, "name", assetName)) continue;
        const bool wanted = source.useArchive ? HasExtension(assetName, ".zip") :
                            (HasExtension(assetName, ".wps") || HasExtension(assetName, ".wms"));
        if (!wanted) continue;

        std::string url;
        std::string digest;
        if (!ExtractJsonString(asset, 0, "browser_download_url", url) ||
            !ExtractJsonString(asset, 0, "digest", digest)) return false;
        if (digest.rfind("sha256:", 0) == 0) digest.erase(0, 7);
        if (!IsSafeName(assetName) || !IsHttps(url) || !IsSha256(digest)) return false;
        server.files.push_back({assetName, url, digest, 0});
    }

    if (source.useArchive) return server.files.size() == 1;
    const bool hasWps = std::any_of(server.files.begin(), server.files.end(), [](const DownloadFile &file) {
        return HasExtension(file.name, ".wps");
    });
    const bool hasWms = std::any_of(server.files.begin(), server.files.end(), [](const DownloadFile &file) {
        return HasExtension(file.name, ".wms");
    });
    return hasWps && hasWms;
}

bool LoadReleaseServers() {
    gServers.clear();
    gReleaseErrors.clear();
    for (const ReleaseSource &source : kReleaseSources) {
        std::string response;
        if (!Request(std::string(kReleaseApiBase) + source.repository + "/releases/latest", response)) {
            gReleaseErrors.push_back(std::string(source.displayName) + ": release lookup failed");
            continue;
        }
        ServerInfo server;
        if (!ParseReleaseAssets(response, source, server)) {
            gReleaseErrors.push_back(std::string(source.displayName) + ": release assets unavailable");
            continue;
        }
        gServers.push_back(std::move(server));
    }
    return !gServers.empty();
}

size_t WriteText(void *data, size_t size, size_t count, void *context) {
    auto *output = static_cast<std::string *>(context);
    output->append(static_cast<const char *>(data), size * count);
    return size * count;
}

size_t WriteFile(void *data, size_t size, size_t count, void *context) {
    return std::fwrite(data, size, count, static_cast<Transfer *>(context)->file);
}

int Progress(void *context, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) {
    auto *transfer = static_cast<Transfer *>(context);
    transfer->percent = total > 0 ? static_cast<int>((current * 100) / total) : 0;
    gProgress = transfer->percent;
    return 0;
}

bool Request(const std::string &url, std::string &result) {
    if (!IsHttps(url) || !Initialize()) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteText);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WiiU-ServerSelector/2.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return code == CURLE_OK;
}

bool EnsureDirectory(const std::string &path) {
    struct stat info{};
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

void RemoveDirectoryFiles(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (!directory) return;
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") unlink((path + "/" + name).c_str());
    }
    closedir(directory);
    rmdir(path.c_str());
}

bool DownloadRemoteFile(const DownloadFile &file, const std::string &path) {
    FILE *output = std::fopen(path.c_str(), "wb");
    if (!output) return false;
    Transfer transfer{output, 0};
    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(output);
        unlink(path.c_str());
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, file.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    std::fclose(output);
    if (code != CURLE_OK) unlink(path.c_str());
    return code == CURLE_OK;
}

uint16_t ReadLittleEndian16(const unsigned char *data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

uint32_t ReadLittleEndian32(const unsigned char *data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool ExtractClientArchive(const std::string &archivePath, const std::string &destination) {
    struct stat info{};
    if (stat(archivePath.c_str(), &info) != 0 || info.st_size < 22 || info.st_size > 32 * 1024 * 1024) {
        return false;
    }

    std::vector<unsigned char> archive(static_cast<size_t>(info.st_size));
    FILE *input = std::fopen(archivePath.c_str(), "rb");
    if (!input) return false;
    const size_t archiveSize = std::fread(archive.data(), 1, archive.size(), input);
    const bool readError = std::ferror(input) != 0;
    std::fclose(input);
    if (readError || archiveSize != archive.size()) return false;

    size_t endRecord = archive.size() - 22;
    while (true) {
        if (ReadLittleEndian32(archive.data() + endRecord) == 0x06054b50) break;
        if (endRecord == 0) return false;
        --endRecord;
    }
    if (ReadLittleEndian16(archive.data() + endRecord + 4) != 0 ||
        ReadLittleEndian16(archive.data() + endRecord + 6) != 0) return false;

    const uint16_t entryCount = ReadLittleEndian16(archive.data() + endRecord + 10);
    const uint32_t directorySize = ReadLittleEndian32(archive.data() + endRecord + 12);
    const uint32_t directoryOffset = ReadLittleEndian32(archive.data() + endRecord + 16);
    if (entryCount == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff ||
        static_cast<uint64_t>(directoryOffset) + directorySize > endRecord) return false;

    bool foundWps = false;
    bool foundWms = false;
    size_t entryPosition = directoryOffset;
    const size_t directoryEnd = static_cast<size_t>(directoryOffset) + directorySize;
    for (uint16_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        if (entryPosition + 46 > directoryEnd ||
            ReadLittleEndian32(archive.data() + entryPosition) != 0x02014b50) return false;

        const unsigned char *entry = archive.data() + entryPosition;
        const uint16_t flags = ReadLittleEndian16(entry + 8);
        const uint16_t method = ReadLittleEndian16(entry + 10);
        const uint32_t expectedCrc = ReadLittleEndian32(entry + 16);
        const uint32_t compressedSize = ReadLittleEndian32(entry + 20);
        const uint32_t uncompressedSize = ReadLittleEndian32(entry + 24);
        const uint16_t nameLength = ReadLittleEndian16(entry + 28);
        const uint16_t extraLength = ReadLittleEndian16(entry + 30);
        const uint16_t commentLength = ReadLittleEndian16(entry + 32);
        const uint32_t localOffset = ReadLittleEndian32(entry + 42);
        const size_t nextEntry = entryPosition + 46 + nameLength + extraLength + commentLength;
        if (nextEntry > directoryEnd) return false;

        std::string archiveName(reinterpret_cast<const char *>(entry + 46), nameLength);
        entryPosition = nextEntry;
        if (archiveName.empty() || archiveName.back() == '/' || archiveName.back() == '\\') continue;
        const size_t baseNameStart = archiveName.find_last_of("/\\");
        const std::string fileName = baseNameStart == std::string::npos ? archiveName :
                                     archiveName.substr(baseNameStart + 1);
        const bool isWps = HasExtension(fileName, ".wps");
        const bool isWms = HasExtension(fileName, ".wms");
        if ((!isWps && !isWms) || !IsSafeName(fileName)) continue;
        if ((isWps && foundWps) || (isWms && foundWms) || (flags & 1) != 0 ||
            (method != 0 && method != 8) || uncompressedSize == 0 ||
            uncompressedSize > 32 * 1024 * 1024 || compressedSize > 32 * 1024 * 1024 ||
            static_cast<uint64_t>(localOffset) + 30 > archive.size()) return false;

        const unsigned char *local = archive.data() + localOffset;
        if (ReadLittleEndian32(local) != 0x04034b50) return false;
        const uint16_t localNameLength = ReadLittleEndian16(local + 26);
        const uint16_t localExtraLength = ReadLittleEndian16(local + 28);
        const size_t dataOffset = static_cast<size_t>(localOffset) + 30 + localNameLength + localExtraLength;
        if (static_cast<uint64_t>(dataOffset) + compressedSize > archive.size()) return false;

        std::vector<unsigned char> extracted(uncompressedSize);
        if (method == 0) {
            if (compressedSize != uncompressedSize) return false;
            std::memcpy(extracted.data(), archive.data() + dataOffset, uncompressedSize);
        } else {
            z_stream stream{};
            stream.next_in = const_cast<Bytef *>(archive.data() + dataOffset);
            stream.avail_in = compressedSize;
            stream.next_out = extracted.data();
            stream.avail_out = uncompressedSize;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
            const int result = inflate(&stream, Z_FINISH);
            const bool valid = result == Z_STREAM_END && stream.total_out == uncompressedSize &&
                               stream.total_in == compressedSize;
            inflateEnd(&stream);
            if (!valid) return false;
        }

        const uLong actualCrc = crc32(crc32(0L, Z_NULL, 0), extracted.data(), uncompressedSize);
        if (actualCrc != expectedCrc) return false;
        const std::string outputPath = destination + "/" + fileName;
        FILE *output = std::fopen(outputPath.c_str(), "wb");
        if (!output) return false;
        const bool wroteFile = std::fwrite(extracted.data(), 1, extracted.size(), output) == extracted.size();
        std::fclose(output);
        if (!wroteFile) {
            unlink(outputPath.c_str());
            return false;
        }
        if (isWps) foundWps = true;
        if (isWms) foundWms = true;
    }
    return entryPosition == directoryEnd && foundWps && foundWms;
}

bool Verify(const std::string &path, const std::string &expected) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file || !IsSha256(expected)) {
        if (file) std::fclose(file);
        return false;
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    unsigned char buffer[16 * 1024];
    while (const size_t count = std::fread(buffer, 1, sizeof(buffer), file)) {
        mbedtls_sha256_update(&context, buffer, count);
        if (count < sizeof(buffer) && std::ferror(file)) {
            std::fclose(file);
            mbedtls_sha256_free(&context);
            return false;
        }
        if (count < sizeof(buffer)) break;
    }
    unsigned char digest[32];
    mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    std::fclose(file);
    char actual[65] = {};
    for (size_t index = 0; index < sizeof(digest); ++index) std::snprintf(actual + index * 2, 3, "%02x", digest[index]);
    std::string normalized = expected;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized == actual;
}

bool Install(const ServerInfo &server) {
    if (!IsSafeName(server.name) || server.files.empty() ||
        !EnsureDirectory("/vol/external01/wiiu") || !EnsureDirectory(kClientsRoot)) return false;

    const std::string staging = std::string(kClientsRoot) + "/.serverselector-download-" + server.name;
    const std::string destination = std::string(kClientsRoot) + "/" + server.name;
    RemoveDirectoryFiles(staging);
    if (mkdir(staging.c_str(), 0777) != 0 && errno != EEXIST) return false;
    for (const DownloadFile &file : server.files) {
        SetStatus(file.name.c_str());
        gProgress = 0;
        const std::string temporary = staging + "/" + file.name;
        if (!IsHttps(file.url) || !DownloadRemoteFile(file, temporary) || !Verify(temporary, file.sha256)) {
            RemoveDirectoryFiles(staging);
            SetStatus("Download failed or checksum mismatch");
            return false;
        }
    }
    for (const DownloadFile &file : server.files) {
        if (!HasExtension(file.name, ".zip")) continue;
        SetStatus("Extracting Inkay files");
        if (!ExtractClientArchive(staging + "/" + file.name, staging)) {
            RemoveDirectoryFiles(staging);
            SetStatus("Invalid Inkay archive");
            return false;
        }
        unlink((staging + "/" + file.name).c_str());
    }
    RemoveDirectoryFiles(destination);
    if (mkdir(destination.c_str(), 0777) != 0 && errno != EEXIST) return false;
    for (const DownloadFile &file : server.files) {
        if (HasExtension(file.name, ".zip")) continue;
        if (rename((staging + "/" + file.name).c_str(), (destination + "/" + file.name).c_str()) != 0) {
            RemoveDirectoryFiles(staging);
            RemoveDirectoryFiles(destination);
            return false;
        }
    }
    if (server.files.size() == 1 && HasExtension(server.files.front().name, ".zip")) {
        DIR *directory = opendir(staging.c_str());
        if (!directory) return false;
        std::vector<std::string> extractedFiles;
        while (dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            extractedFiles.push_back(name);
        }
        closedir(directory);
        for (const std::string &name : extractedFiles) {
            if (rename((staging + "/" + name).c_str(), (destination + "/" + name).c_str()) != 0) {
                RemoveDirectoryFiles(staging);
                RemoveDirectoryFiles(destination);
                return false;
            }
        }
    }
    RemoveDirectoryFiles(staging);
    gProgress = 100;
    SetStatus("Server installed");
    return true;
}

int Worker(int, const char **) {
    const bool success = Install(gPendingServer);
    OSReport("[ServerDownloader] %s: %s\n", gPendingServer.name.c_str(), success ? "complete" : "failed");
    gBusy = false;
    return 0;
}

void DeleteItem(void *context) {
    auto *item = static_cast<DownloadItem *>(context);
    if (!item) return;
    std::free(item->identifier);
    delete item;
}

void DownloadSelected(void *context, WUPSConfigSimplePadData input) {
    auto *item = static_cast<DownloadItem *>(context);
    if (!item || gBusy || !(input.buttons_d & WUPS_CONFIG_BUTTON_A)) return;
    gPendingServer = item->server;
    gBusy = true;
    SetStatus("Starting download...");
    OSCreateThread(&gWorkerThread, Worker, 0, nullptr, gWorkerStack + sizeof(gWorkerStack), sizeof(gWorkerStack), 16, 0);
    OSResumeThread(&gWorkerThread);
}

int32_t StatusDisplay(void *, char *buffer, int32_t size) {
    if (buffer && size > 0) std::snprintf(buffer, size, "%s%s", gStatus, gBusy ? "..." : "");
    return 0;
}

int32_t DownloadDisplay(void *, char *buffer, int32_t size) {
    if (buffer && size > 0) std::snprintf(buffer, size, "%s", gBusy ? "Busy" : "Download");
    return 0;
}

int32_t VersionDisplay(void *, char *buffer, int32_t size) {
    if (buffer && size > 0) std::snprintf(buffer, size, "%s", gVersionStatus);
    return 0;
}

int32_t VersionSelectedDisplay(void *, char *buffer, int32_t size) {
    if (buffer && size > 0) std::snprintf(buffer, size, "Check now");
    return 0;
}

void DeleteVersionItem(void *context) {
    auto *item = static_cast<VersionItem *>(context);
    if (!item) return;
    std::free(item->identifier);
    delete item;
}

void CheckVersion(void *, WUPSConfigSimplePadData input) {
    if (!(input.buttons_d & WUPS_CONFIG_BUTTON_A) || gBusy) return;

    SetVersionStatus("Checking version...");
    std::string response;
    if (!Request(kPluginReleaseUrl, response)) {
        SetVersionStatus("Version check failed");
        return;
    }

    std::string remoteVersion;
    ParsedVersion remoteParsed;
    ParsedVersion localParsed;
    if (!ExtractJsonString(response, 0, "tag_name", remoteVersion) ||
        !ParseVersion(remoteVersion, remoteParsed) || !ParseVersion(kBuildVersion, localParsed)) {
        SetVersionStatus("Invalid latest release version");
        return;
    }

    std::vector<std::string> assets;
    bool hasValidPluginAsset = false;
    if (ExtractJsonObjects(response, "assets", assets)) {
        for (const std::string &asset : assets) {
            std::string name;
            std::string url;
            std::string digest;
            if (!ExtractJsonString(asset, 0, "name", name) || name != "wiiuserverselector.wps") continue;
            if (!ExtractJsonString(asset, 0, "browser_download_url", url) ||
                !ExtractJsonString(asset, 0, "digest", digest)) break;
            if (digest.rfind("sha256:", 0) == 0) digest.erase(0, 7);
            hasValidPluginAsset = IsHttps(url) && IsSha256(digest);
            break;
        }
    }
    if (!hasValidPluginAsset) {
        SetVersionStatus("Latest plugin package unavailable");
        return;
    }

    std::string releaseNotes;
    std::string compatibility;
    ExtractReleaseDetails(response, releaseNotes, compatibility);
    const int comparison = CompareVersions(remoteParsed, localParsed);
    std::string status;
    if (comparison > 0) {
        status = std::string("Update available: ") + remoteVersion;
    } else if (comparison < 0) {
        status = std::string("Dev build: ") + kBuildVersion;
    } else {
        status = std::string("Up to date: ") + remoteVersion;
    }
    SetVersionStatus(status + " | " + releaseNotes + " | " + compatibility);
}

void AddStatusItem(WUPSConfigCategoryHandle category, const std::string &text) {
    WUPSConfigItemStub_AddToCategory(category, text.c_str());
}

} // namespace

bool Initialize() {
    if (gCurlReady) return true;
    gCurlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return gCurlReady;
}

bool DownloadServer(const ServerInfo &server) {
    if (gBusy) return false;
    gBusy = true;
    const bool result = Install(server);
    gBusy = false;
    return result;
}

void AddDownloadServersMenu(WUPSConfigCategoryHandle rootHandle) {
    WUPSConfigCategoryHandle downloadHandle{};
    WUPSConfigAPICreateCategoryOptionsV1 categoryOptions = {
        .name = "Download latest Inkay",
    };
    if (WUPSConfigAPI_Category_Create(categoryOptions, &downloadHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return;
    }

    auto addCategory = [&]() {
        if (WUPSConfigAPI_Category_AddCategory(rootHandle, downloadHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_Destroy(downloadHandle);
        }
    };

    if (!Initialize()) {
        AddStatusItem(downloadHandle, "Network download is unavailable");
        addCategory();
        return;
    }
    if (!LoadReleaseServers()) {
        if (gReleaseErrors.empty()) AddStatusItem(downloadHandle, "No Inkay releases available");
        for (const std::string &error : gReleaseErrors) AddStatusItem(downloadHandle, error);
        addCategory();
        return;
    }
    for (const std::string &error : gReleaseErrors) AddStatusItem(downloadHandle, error);

    for (const ServerInfo &server : gServers) {
        AddStatusItem(downloadHandle, server.name + " notes: " + server.releaseNotes);
        AddStatusItem(downloadHandle, server.name + " compatibility: " + server.compatibility);
        auto *item = new (std::nothrow) DownloadItem();
        if (!item) continue;
        item->server = server;
        const std::string identifier = "download_" + server.name;
        item->identifier = ::strdup(identifier.c_str());
        if (!item->identifier) {
            delete item;
            continue;
        }
        std::string title = server.name + " (" + server.version + ")";
        WUPSConfigAPIItemCallbacksV2 callbacks = {
            .getCurrentValueDisplay = StatusDisplay,
            .getCurrentValueSelectedDisplay = DownloadDisplay,
            .onSelected = nullptr,
            .restoreDefault = nullptr,
            .isMovementAllowed = nullptr,
            .onCloseCallback = nullptr,
            .onInput = DownloadSelected,
            .onInputEx = nullptr,
            .onDelete = DeleteItem,
        };
        WUPSConfigAPIItemOptionsV2 options = {title.c_str(), item, callbacks};
        if (WUPSConfigAPI_Item_Create(options, &item->handle) == WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_AddItem(downloadHandle, item->handle);
            gItems.push_back(item);
        } else {
            DeleteItem(item);
        }
    }
    addCategory();
}

void AddVersionStatusMenu(WUPSConfigCategoryHandle rootHandle) {
    auto *item = new (std::nothrow) VersionItem();
    if (!item) return;
    item->identifier = ::strdup("plugin_version");
    if (!item->identifier) {
        delete item;
        return;
    }

    WUPSConfigAPIItemCallbacksV2 callbacks = {
        .getCurrentValueDisplay = VersionDisplay,
        .getCurrentValueSelectedDisplay = VersionSelectedDisplay,
        .onSelected = nullptr,
        .restoreDefault = nullptr,
        .isMovementAllowed = nullptr,
        .onCloseCallback = nullptr,
        .onInput = CheckVersion,
        .onInputEx = nullptr,
        .onDelete = DeleteVersionItem,
    };
    WUPSConfigAPIItemOptionsV2 options = {
        .displayName = "Plugin version",
        .context = item,
        .callbacks = callbacks,
    };
    if (WUPSConfigAPI_Item_Create(options, &item->handle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        DeleteVersionItem(item);
        return;
    }
    WUPSConfigAPI_Category_AddItem(rootHandle, item->handle);
}

} // namespace ServerDownloader
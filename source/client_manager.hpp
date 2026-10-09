#pragma once

#include <string>
#include <vector>

namespace ClientManager {

struct Client {
    std::string name;
    std::string path;
    std::string identifier;
    bool active = false;
};

std::vector<Client> Scan();
bool Install(const std::string &clientPath);

} // namespace ClientManager
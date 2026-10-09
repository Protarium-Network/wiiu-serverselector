#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <wups.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <coreinit/debug.h>
#include <coreinit/launch.h>
#include <coreinit/thread.h>

#include "boot_notification.hpp"
#include "client_manager.hpp"
#include "server_downloader.hpp"

WUPS_PLUGIN_NAME("Wii U Server Selector/Downloader");
WUPS_PLUGIN_DESCRIPTION("Download and select Wii U server clients");
WUPS_PLUGIN_VERSION("v1.0.2");
WUPS_PLUGIN_AUTHOR("hadley557 and NoobieDoesModding");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_STORAGE("WiiUClientSelector");
WUPS_USE_WUT_DEVOPTAB();

namespace {

struct SelectionItem {
    WUPSConfigItemHandle handle{};
    char *identifier = nullptr;
    std::string path;
    bool selected = false;
};

OSThread gSelectionThread;
alignas(8) uint8_t gSelectionStack[4096];
std::string gPendingClient;
OSThread gBootNotificationThread;
alignas(8) uint8_t gBootNotificationStack[4096];

int BootNotificationWorker(int, const char **) {
    OSSleepTicks(OSMillisecondsToTicks(2000));

    const std::vector<ClientManager::Client> clients = ClientManager::Scan();
    std::string selectedNetwork;
    for (const auto &client : clients) {
        if (client.active) {
            selectedNetwork = client.name;
            break;
        }
    }
    BootNotification::ShowSelectedNetwork(selectedNetwork);
    return 0;
}

int SelectionWorker(int, const char **) {
    const bool installed = ClientManager::Install(gPendingClient);
    OSReport("[ServerSelector] client install %s\n", installed ? "completed" : "failed");
    if (installed) {
        OSSleepTicks(OSMillisecondsToTicks(1500));
        OSLaunchTitlev(OS_TITLE_ID_REBOOT, 0, nullptr);
    }
    return 0;
}

int32_t GetCurrentDisplay(void *, char *buffer, int32_t size) {
    if (buffer && size > 0) buffer[0] = '\0';
    return 0;
}

int32_t GetSelectedDisplay(void *context, char *buffer, int32_t size) {
    if (!buffer || size <= 0) return 0;
    auto *item = static_cast<SelectionItem *>(context);
    std::snprintf(buffer, size, "%s", item && item->selected ? "Selected" : "Select \xEE\x80\x80");
    return 0;
}

void DeleteSelectionItem(void *context) {
    auto *item = static_cast<SelectionItem *>(context);
    if (!item) return;
    std::free(item->identifier);
    delete item;
}

void SelectClient(void *context, WUPSConfigSimplePadData input) {
    auto *item = static_cast<SelectionItem *>(context);
    if (!item || item->selected || !(input.buttons_d & WUPS_CONFIG_BUTTON_A)) return;

    gPendingClient = item->path;
    OSCreateThread(&gSelectionThread, SelectionWorker, 0, nullptr,
                   gSelectionStack + sizeof(gSelectionStack), sizeof(gSelectionStack), 16, 0);
    OSResumeThread(&gSelectionThread);
}

bool AddClientItem(WUPSConfigCategoryHandle category, const ClientManager::Client &client) {
    auto item = std::unique_ptr<SelectionItem>(new (std::nothrow) SelectionItem());
    if (!item) return false;

    item->identifier = ::strdup(client.identifier.c_str());
    if (!item->identifier) return false;
    item->path = client.path;
    item->selected = client.active;

    std::string title = client.name;
    if (client.active) title += " (selected)";

    WUPSConfigAPIItemCallbacksV2 callbacks = {
        .getCurrentValueDisplay = GetCurrentDisplay,
        .getCurrentValueSelectedDisplay = GetSelectedDisplay,
        .onSelected = nullptr,
        .restoreDefault = nullptr,
        .isMovementAllowed = nullptr,
        .onCloseCallback = nullptr,
        .onInput = SelectClient,
        .onInputEx = nullptr,
        .onDelete = DeleteSelectionItem,
    };
    WUPSConfigAPIItemOptionsV2 options = {
        .displayName = title.c_str(),
        .context = item.get(),
        .callbacks = callbacks,
    };

    WUPSConfigItemHandle handle{};
    if (WUPSConfigAPI_Item_Create(options, &handle) != WUPSCONFIG_API_RESULT_SUCCESS) return false;
    item->handle = handle;
    WUPSConfigAPI_Category_AddItem(category, item.release()->handle);
    return true;
}

void AddMessageItem(WUPSConfigCategoryHandle category, const char *message) {
    WUPSConfigItemStub_AddToCategory(category, message);
}

void AddHelpMenu(WUPSConfigCategoryHandle rootHandle) {
    WUPSConfigCategoryHandle helpHandle{};
    WUPSConfigAPICreateCategoryOptionsV1 categoryOptions = {
        .name = "Help and version",
    };
    if (WUPSConfigAPI_Category_Create(categoryOptions, &helpHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return;
    }

    ServerDownloader::AddVersionStatusMenu(helpHandle);

    const char *instructions[] = {
        "Manual server installation",
        "1. Download the server .wps and .wms files.",
        "2. On the SD card, create:",
        "   sd:/wiiu/wiiu-clients/<NetworkName>/",
        "3. Put both files inside that folder.",
        "4. Reinsert the SD card and reopen this menu.",
        "5. Select the server from the main menu.",
        "6. The Wii U will reboot after installation.",
        "",
        "More details:",
        "github.com/Protarium-Network/wiiu-serverselector",
    };
    for (const char *instruction : instructions) AddMessageItem(helpHandle, instruction);

    if (WUPSConfigAPI_Category_AddCategory(rootHandle, helpHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        WUPSConfigAPI_Category_Destroy(helpHandle);
    }
}

void AddClientsMenu(WUPSConfigCategoryHandle rootHandle, const std::vector<ClientManager::Client> &clients) {
    WUPSConfigCategoryHandle clientsHandle{};
    WUPSConfigAPICreateCategoryOptionsV1 categoryOptions = {
        .name = "Installed clients",
    };
    if (WUPSConfigAPI_Category_Create(categoryOptions, &clientsHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return;
    }

    if (clients.empty()) {
        AddMessageItem(clientsHandle, "No downloaded clients found");
    } else {
        for (const auto &client : clients) AddClientItem(clientsHandle, client);
    }

    if (WUPSConfigAPI_Category_AddCategory(rootHandle, clientsHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        WUPSConfigAPI_Category_Destroy(clientsHandle);
    }
}

} // namespace

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    const std::vector<ClientManager::Client> clients = ClientManager::Scan();
    AddClientsMenu(rootHandle, clients);
    ServerDownloader::AddDownloadServersMenu(rootHandle);
    AddHelpMenu(rootHandle);
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    WUPSStorageAPI::SaveStorage(false);
}

INITIALIZE_PLUGIN() {
    OSCreateThread(&gBootNotificationThread, BootNotificationWorker, 0, nullptr,
                   gBootNotificationStack + sizeof(gBootNotificationStack),
                   sizeof(gBootNotificationStack), 16, 0);
    OSResumeThread(&gBootNotificationThread);

    WUPSConfigAPIOptionsV1 options = {.name = "Wii U Server Selector"};
    WUPSConfigAPI_Init(options, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}
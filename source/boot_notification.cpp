#include "boot_notification.hpp"

#include <cstdint>
#include <cstdio>

#include <coreinit/debug.h>
#include <coreinit/dynload.h>

namespace BootNotification {
namespace {

struct NMColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

using NotificationStatus = int32_t;
using NotificationType = int32_t;
using AddStaticNotification = NotificationStatus (*)(
    const char *, NotificationType, float, float, NMColor, NMColor,
    void (*)(uint32_t, void *), void *, bool);

constexpr const char *kNotificationModule = "homebrew_notifications";
constexpr NotificationType kInfoNotification = 0;

} // namespace

void ShowSelectedNetwork(const std::string &networkName) {
    OSDynLoad_Module module = nullptr;
    if (OSDynLoad_Acquire(kNotificationModule, &module) != OS_DYNLOAD_OK) {
        OSReport("[ServerSelector] NotificationModule is unavailable.\n");
        return;
    }

    AddStaticNotification addNotification = nullptr;
    if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC,
                             "NMAddStaticNotificationV2",
                             reinterpret_cast<void **>(&addNotification)) != OS_DYNLOAD_OK ||
        !addNotification) {
        OSReport("[ServerSelector] NotificationModule API is unavailable.\n");
        return;
    }

    char message[128] = {};
    if (networkName.empty()) {
        std::snprintf(message, sizeof(message), "Wii U Server Selector: no network selected");
    } else {
        std::snprintf(message, sizeof(message), "Wii U Server Selector: %s selected", networkName.c_str());
    }

    const NotificationStatus status = addNotification(
        message,
        kInfoNotification,
        5.0f,
        0.0f,
        {255, 255, 255, 255},
        {40, 85, 145, 255},
        nullptr,
        nullptr,
        true);
    if (status != 0) {
        OSReport("[ServerSelector] failed to queue boot notification: %d\n", status);
    }
}

} // namespace BootNotification
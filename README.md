# Wii U Server Selector

A WUPS plugin for Aroma that downloads server client packages and switches the
installed `.wps` and `.wms` files used by Wii U applications.

When the plugin initializes during boot, it checks the installed client files
and queues a notification such as `Wii U Server Selector: Pretendo Network
selected`. The notification uses the optional Aroma NotificationModule; without
that module, the selector continues to work and writes the result to the system
log instead.

The compiled version is `1.0.2`. Open `Help and version`, then select
`Plugin version` and press A to check the GitHub Releases API for the latest
plugin release. The menu reports whether the installed build is current or an
update is available; it does not download or install the plugin update.

The menu separates `Download latest Inkay`, `Installed clients`, and
`Help and version`. The help submenu includes manual installation steps. The
reference instructions are maintained at:

```text
https://raw.githubusercontent.com/Protarium-Network/wiiu-serverselector/refs/heads/main/README.md
```

Inkay downloads are listed by project and latest GitHub release version.
The download menu also shows a short summary of each release and any
compatibility requirement stated in its release notes. If a publisher does not
state a requirement, the menu says so. Selecting a release downloads and
verifies its assets before adding it to `Installed clients`.

The plugin version check includes the latest plugin release notes and any
compatibility requirement stated by that release. It remains an informational
check and does not download or install plugin updates.

## Layout

- `source/main_plugin.cpp` contains plugin metadata and the WUPS configuration UI.
- `source/client_manager.cpp` scans, verifies, and installs local client folders.
- `source/downloader.cpp` loads GitHub release assets, downloads files to a temporary staging folder,
  verifies SHA-256 checksums, and installs complete server packages.
- `source/server_downloader.hpp` and `source/client_manager.hpp` are the small
  interfaces between those components.

## Client folders

Downloaded clients are stored at:

```text
sd:/wiiu/wiiu-clients/<Server Name>/*.wps
sd:/wiiu/wiiu-clients/<Server Name>/*.wms
```

The plugin compares file contents against Aroma's installed module and plugin
files. Selecting a client replaces all matching installed `.wps` and `.wms`
files, then reboots the console.

For manual installation, place both server files in the server's folder, reopen
the plugin menu, select that server, and allow the Wii U to reboot.

## Network downloads

The downloader uses the GitHub Releases API's `/releases/latest` endpoint for
`PretendoNetwork/Inkay` and
`Protarium-Network/Inkay-GitHub-Release`. It verifies each selected asset using
GitHub's SHA-256 digest. The official Pretendo release is a ZIP; only its
`.wps` and `.wms` files are extracted, with ZIP CRC validation. Protarium's
release supplies those files directly. Downloads use a temporary folder inside
`wiiu-clients`, and incomplete packages are removed rather than exposed as
installable clients.
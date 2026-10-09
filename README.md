# Wii U Server Selector

A plugin for Aroma that downloads and installs Wii U server client packages, then swaps the active `.wps` and `.wms` files used by supported apps.

## Features

- Checks installed clients at boot and shows the selected server in the Aroma notification system when available
- Lets users download the latest Inkay release from GitHub
- Verifies downloaded files before installing them
- Stores installed clients in a per-server folder under `sd:/wiiu/wiiu-clients`
- Checks the current plugin version against the latest GitHub release and shows release notes

## Installed clients

Downloaded client folders are stored in:

```text
sd:/wiiu/wiiu-clients/<Server Name>/
```

Each server folder contains its `.wps` and `.wms` files. Selecting a client in the menu replaces the matching installed files and reboots the console.

## Manual install

Place both server files in the matching server folder, reopen the plugin menu, and select the server to apply it.

## Notes

- The downloader reads GitHub Releases metadata and verifies asset hashes before installation.
- Incomplete or invalid downloads are discarded instead of being exposed as installable clients.
- The plugin can work without Aroma notifications; in that case it logs the result instead.
# F4 Online

A two-player co-op mod for Fallout 4 (1.11.240, Steam): an F4SE plugin built on CommonLibF4.

- Shared kills, damage and XP
- The other player appears as a peaceful ally with their name, real face, outfit and weapon
- Walking animation and smooth movement
- Giving items: drop an item next to the other player

## Install
Players get the setup kit from the host. The kit installs F4SE, Address Library (downloaded by the player from
Nexus) and F4 Online, and makes a "Fallout 4 (F4SE)" desktop shortcut. That shortcut runs
`updater/F4Online-Update.ps1`, which downloads the newest `F4Online.dll` from this repo's Releases before starting the game.

## Build
```bat
git clone --recurse-submodules https://github.com/fentmaxxer694201-byte/F4Online
xmake build -y
```
Output: `build/windows/x64/releasedbg/F4Online.dll`.

## License
GPL-3.0 with the modding exception in `EXCEPTIONS`. Based on the libxse CommonLibF4 plugin template.

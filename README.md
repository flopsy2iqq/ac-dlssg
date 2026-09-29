[![English](https://img.shields.io/badge/lang-English-blue)](README.md) [![Русский](https://img.shields.io/badge/lang-Русский-red)](README.ru.md) [![Install](https://img.shields.io/badge/-Install-brightgreen)](docs/INSTALL.md)

# ac-dlssg

NVIDIA DLSS Frame Generation (DLSS-G 2X) for **Assetto Corsa** with **Custom Shaders Patch**.

**Status: test build. Frame generation works in game.** On the test PC the frame rate doubles with no ghosting and no blur. The pictures are in [Screenshots](#screenshots).

Assetto Corsa renders with DirectX 11. DLSS Frame Generation runs only on DirectX 12 and Vulkan, through NVIDIA Streamline. ac-dlssg connects the two:

- The game keeps rendering in DirectX 11.
- Each finished frame is presented through a DirectX 12 swap chain created with Streamline, and DLSS-G inserts a generated frame between every two real ones.
- Depth and motion vectors come from the DLSS upscaling pass that CSP already runs.
- The camera comes from a small CSP Lua app that the installer adds.

## Screenshots

DLSS-G 2X in Assetto Corsa with CSP:

- **Top left:** CSP's FPS counter. It shows the **real** frames the game renders.
- **Top right:** the NVIDIA overlay. It shows the frames **on screen after frame generation**.

Test PC: Intel Core i9-9900 (8 cores, 16 threads), GeForce RTX 3080 10 GB (driver 616.64), 16 GB DDR4-3600, Windows 10 22H2, CSP 0.3.0-preview622, dlssg_for_sm86 0.3.5.

![DLSS-G 2X, cockpit view: 62 real fps, 124 fps on screen](docs/screenshots/fg2x-1.jpg)
![DLSS-G 2X, chase view: 55 real fps, 112 fps on screen](docs/screenshots/fg2x-2.jpg)
![DLSS-G 2X, night: 53 real fps, 105 fps on screen](docs/screenshots/fg2x-3.jpg)
![DLSS-G 2X, side view at sunset: 58 real fps, 116 fps on screen](docs/screenshots/fg2x-4.jpg)
![DLSS-G 2X, cockpit in rain: 46 real fps, 90 fps on screen](docs/screenshots/fg2x-5.jpg)
![DLSS-G 2X, front view: 55 real fps, 108 fps on screen](docs/screenshots/fg2x-6.jpg)
![DLSS-G 2X, night with CSP's render stats: 59 real fps, 119 fps on screen](docs/screenshots/fg2x-7.jpg)

### Tested on a laptop

A friend's Acer Nitro 5 AN515-57: GeForce RTX 3050 Ti Laptop GPU (4 GB, driver 610.88), Core i5-11400H, 32 GB RAM, Windows 11 25H2, Optimus (the Intel iGPU drives the panel), no ReShade (standalone mode), CSP preview634. 31 real fps become 60 on screen.

![DLSS-G 2X on an RTX 3050 Ti laptop: 31 real fps, 60 fps on screen](docs/screenshots/fg2x-laptop-1.jpg)
![DLSS-G 2X on an RTX 3050 Ti laptop in a tunnel: 30 real fps, 57 fps on screen](docs/screenshots/fg2x-laptop-2.jpg)

On a 4 GB card the video memory is tight: if the game stutters, lower CSP's texture or shadow quality.

## Requirements

- **Assetto Corsa** from Steam with **Custom Shaders Patch 0.3.0** or newer. It was tested with preview622 and preview634.
- **CSP's DLSS upscaling turned on**, in any quality mode including DLAA. Frame generation takes its depth and motion vectors from it.
- **An NVIDIA RTX GPU:**
  - RTX 40 and RTX 50 support DLSS-G themselves.
  - RTX 30, including laptop GPUs such as the RTX 3050 Ti, needs the third-party [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86). You do not install it yourself: on an RTX 30 the installer downloads the pinned release 0.3.5 from the author's repository, checks its hashes and puts it next to `acs.exe`. See [RTX 30 and dlssg_for_sm86](#rtx-30-and-dlssg_for_sm86). It needs NVIDIA driver R580 or newer.
  - RTX 20 is not supported by this project.
- **Windows 10 version 2004 or newer, or Windows 11.** Hardware-accelerated GPU scheduling must be on: Settings > System > Display > Graphics > Change default graphics settings.
- **Laptops:** set `acs.exe` to "High performance" in the same Graphics settings, so the game runs on the NVIDIA GPU.
- **Internet on the first install.** The installer downloads NVIDIA Streamline 2.14.1 (about 276 MB) from NVIDIA's GitHub release and checks its hash and NVIDIA's signatures, and on an RTX 30 also dlssg_for_sm86 (about 30 MB). This project never ships NVIDIA files or dlssg_for_sm86.
- **ReShade is optional.** With ReShade 6.8+ (add-on build) installed as `dxgi.dll`, the bridge loads through ReShade. Without it, the bridge itself is installed as `dxgi.dll`. The installer picks the mode.

## Install

1. Close Assetto Corsa and Content Manager's game session.
2. Download `ac-dlssg-<version>.zip` from [Releases](https://github.com/flopsy2iqq/ac-dlssg/releases) and extract it anywhere. The folder holds `install.bat` and the folders `docs`, `files`, `scripts` and `tools`.
3. Double-click `install.bat`. That is all: the installer asks nothing. It finds the game through Steam, downloads Streamline (and on an RTX 30 dlssg_for_sm86), prints where NVIDIA's license files are (installing means you accept them) and installs. If the game is under `C:\Program Files (x86)`, Windows asks for administrator rights; confirm, and the install goes on in a new window. At the end the window waits for Enter.
4. Start the game as usual and make sure DLSS is the upscaler in CSP's graphics settings.
5. Drive. Frame generation turns on by itself once CSP's DLSS runs; it stays off in the pause menu and the main menu.

**Upgrading:** run `install.bat` of the new package over the old install. It replaces the files of the old build, removes the ones the new build no longer ships, and switches between ReShade and standalone mode by itself when ReShade was installed or removed since. It stops only for files that are clearly not this project's, such as another mod's `dxgi.dll`.

**Advanced:** `install.bat -NoSpoof` installs without dlssg_for_sm86; on an RTX 30 frame generation then does not run.

## Use

- **Ctrl+F10** switches frame generation on and off, for an A/B comparison of the same scene.
- The **AC DLSS-G** window (CSP's app bar, in both install modes) has the same switch, shows why frame generation is off, the real and output fps, the bridge's GPU time and video memory, and **Save as default** keeps the current switches in `ac-dlssg.ini`.
- CSP's own FPS counter shows real frames. Use the NVIDIA overlay (Alt+R), RTSS or another external counter to see the frames on screen.
- Settings are in `<game>\ac-dlssg\ac-dlssg.ini`: `start_with_fg`, `hotkey` and `log_level`.

## Uninstall

Double-click `tools\uninstall.bat`. It restores every file the installer changed, and removes the bridge, the Streamline files, the Lua app and the dlssg_for_sm86 files the installer put there; a dlssg_for_sm86 you had before stays. Logs and settings stay in `<game>\ac-dlssg` unless you run `tools\uninstall.bat -RemoveData`, which also deletes dlssg_for_sm86's logs and cache when the installer installed it.

## If something goes wrong

- **The game starts but the frame rate does not change.** Look at `<game>\ac-dlssg\logs\bridge.log`. The line `fg: DLSS-G on` means frame generation runs. A line `fg: frame without DLSS-G: <reason>`, or `DLSS-G is not supported on this adapter (...)`, says why it does not.
- **The game crashes or does not start.** Run `tools\uninstall.bat`; the game is then back to how it was.
- **Reporting a problem:** double-click `tools\collect-logs.bat` after the game is closed. It only reads files and writes one zip into the `tools` folder. Attach that zip to a [GitHub issue](https://github.com/flopsy2iqq/ac-dlssg/issues).

## Known limitations

- 2X only: one generated frame per real frame.
- CSP app windows and on-screen UI can show small artifacts during fast motion. A HUD-less colour buffer is planned.
- The following are not supported: VR, triple screens, HDR output, MSAA, `EXCLUSIVE_FULLSCREEN=1`, and letterboxed output.
- Other CSP upscalers (FSR, XeSS) do not provide the inputs frame generation needs.

## RTX 30 and dlssg_for_sm86

NVIDIA locks DLSS Frame Generation to RTX 40 and newer. [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) by sdli1995, which includes Coldwood1026's RTX 20 work, lifts that lock on RTX 30. The author agreed to this project pointing to it.

- ac-dlssg does not contain or modify it. On an RTX 30 the installer downloads exactly `version.dll` and `dlssg_sm86.ini` of release 0.3.5 (commit `9621db5`) from the author's repository on your PC, checks their git hashes (and the SHA-256 of `version.dll`) before it installs anything, and prints this notice first. A `version.dll` that is already in the game folder is never replaced.
- That repository has no LICENSE file. Its README says the source is GPLv3, but no source is published.
- It embeds NVIDIA's `nvngx_dlssg.dll`, which is not relicensed.
- Using it on RTX 30 circumvents a technical limitation, which section 4.d of the NVIDIA RTX SDKs License forbids for that license's licensees.

Whether to use it is your decision: `install.bat -NoSpoof` installs without it, and `tools\uninstall.bat` removes it again.

## How it works (short)

- `ac-dlssg.dll` (or `dxgi.dll` in standalone mode) exports the DXGI entry points and replaces CSP's DirectX 11 swap chain with a proxy.
- Every frame is copied through a shared texture to a DirectX 12 swap chain created through NVIDIA Streamline 2.14.1, with Reflex and PCL markers.
- CSP's DLSS evaluate call is hooked. Its depth and motion vectors are copied into shared textures and tagged for DLSS-G.
- The CSP Lua app `AcDlssg` writes the camera into shared memory. The bridge builds Streamline's camera constants from it.

The full design is in [docs/superpowers/specs](docs/superpowers/specs), and the milestone records are in [docs/superpowers/plans](docs/superpowers/plans).

## Building from source

Requirements: Visual Studio 2022 Build Tools (MSVC 14.44), Windows SDK 10.0.22621 and CMake.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\acdb_tests.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-test-package.ps1
```

`fetch-deps.ps1` stages the pinned Streamline SDK and NVAPI headers into `deps\`, which is git-ignored.

## Credits

- [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) by sdli1995, with Coldwood1026's work: DLSS-G on RTX 30. The installer downloads it from the author's repository on your PC.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline): the SDK that delivers DLSS Frame Generation and Reflex. Its DLLs are downloaded from NVIDIA's release on your PC.
- [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (MIT): the approach for hooking CSP's DLSS call.
- [open-shaders](https://github.com/alandtse/open-shaders) and [Community Shaders](https://github.com/doodlum/skyrim-community-shaders): the DirectX 11 game plus DirectX 12 Streamline swap chain design.

## License

GPL-3.0. See [LICENSE](LICENSE).

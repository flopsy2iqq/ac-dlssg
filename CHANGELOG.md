# Changelog

All notable changes to ac-dlssg are listed here. Versions follow
[Semantic Versioning](https://semver.org/). The release workflow publishes the
section of a version as its GitHub release notes.

## [1.0.0] - 2026-09-29

The first release. ac-dlssg brings NVIDIA DLSS Frame Generation (DLSS-G) to
Assetto Corsa with Custom Shaders Patch (CSP). The game keeps rendering in
DirectX 11. Every finished frame is presented through a DirectX 12 swap chain
created with NVIDIA Streamline 2.14.1, where DLSS-G adds the generated frames.
Depth and motion vectors come from the DLSS upscaling pass that CSP already
runs, and the camera comes from the CSP Lua app `AcDlssg` that the installer
adds.

### Features

- **DLSS-G 2X, 3X and 4X through Streamline.** 2X is the default. `fg_multiplier=3` or `4` in `ac-dlssg.ini` asks for two or three generated frames per real frame. Streamline allows that when it reports more than one generated frame: on RTX 50 natively, and on RTX 30 through dlssg_for_sm86. RTX 40 stays at 2X. A request above what Streamline allows is lowered to its maximum and logged once.
- **NVIDIA Reflex Low Latency** and the PCL latency markers, through Streamline.
- **Automatic on and off.** Frame generation turns on by itself once CSP's DLSS runs, and it stays off in the pause menu and the main menu. Ctrl+F10 (the `hotkey` key) switches it on and off for an A/B comparison.
- **The AC DLSS-G window** in CSP's app bar, in both install modes. It has the same switch, and it shows why frame generation is off (or "On, but paused" while Streamline pauses it for an unfocused game window), the real and output fps, the bridge's GPU time, the video memory used and its budget, the GPU and the dlssg_for_sm86 state. **Save as default** writes the current switches into `ac-dlssg.ini` key by key and keeps every other byte.
- **ReShade mode and standalone mode.** With ReShade 6.8 or newer (the add-on build) installed as `dxgi.dll`, ReShade loads the bridge as its `ProxyLibrary`. Without ReShade, the bridge itself is the game folder's `dxgi.dll`. The installer picks the mode.
- **Laptops with Optimus**, where the NVIDIA GPU renders and the integrated GPU drives the panel. Hardware-accelerated GPU scheduling is read per adapter through D3DKMT, with the registry value only as a fallback. A swap chain on a non-NVIDIA adapter passes through, and the reason names the Windows graphics preference and CSP's `SELECT_ADAPTER`. On Windows 11 25H2, which binds `acs.exe`'s `VERSION.dll` import to System32, the bridge loads the pinned dlssg_for_sm86 `version.dll` by full path.
- **Video memory guard.** Before DLSS-G is first enabled, and whenever the multiplier changes, the bridge asks Streamline how much video memory DLSS-G needs. DLSS-G stays off while that estimate plus `fg_vram_headroom_mib` (default 0) does not fit into the budget, and a 3X or 4X that does not fit falls back to 2X with a log line. The window shows the reason and the video memory.
- **Pass-through.** When frame generation cannot run (no supported GPU, missing files, unsupported CSP settings or a failed Streamline start), the bridge passes through and the game runs as it does without the mod. With `enabled=0` Streamline is never loaded.
- **Logs.** `<game>\ac-dlssg\logs\bridge.log` has a start banner with the versions and the decisions, one statistics line per second and every state change; the previous run's log is kept as `bridge.prev.log`. `ac-dlssg.ini` warns about a key that appears twice and names the value it used.
- **Automatic installer.** Double-click `install.bat`; it asks nothing. It finds the game through Steam, starts itself again with administrator rights when the game folder needs them, downloads Streamline 2.14.1 from NVIDIA's GitHub release and checks its SHA-256 and NVIDIA's signatures, and prints where NVIDIA's license files are.
- **dlssg_for_sm86 on RTX 30.** On an RTX 30 GPU (desktop or laptop) the installer prints a notice about [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) by sdli1995, then downloads exactly `version.dll` and `dlssg_sm86.ini` of release 0.3.5 (commit `9621db5`) from the author's repository, checks their git hashes and the SHA-256 of `version.dll`, and puts them next to `acs.exe`. A `version.dll` that is already there is never replaced. `install.bat -NoSpoof` installs without it.
- **Journaled install, upgrade and uninstall.** Every change is recorded before it is made and rolled back when the install fails. Running `install.bat` of a new package over an earlier install upgrades it without questions: it replaces the old build's files, removes the ones the new build no longer ships, and switches between ReShade and standalone mode when ReShade was added or removed. It stops only for files that are not this project's, such as another mod's `dxgi.dll`. `tools\uninstall.bat` restores every file the installer changed; `-RemoveData` also deletes the logs and settings. `tools\collect-logs.bat` zips the logs for a bug report and only reads files.
- **No re-hosted files.** The release zip holds no NVIDIA file and no dlssg_for_sm86 file; both are downloaded on the player's PC. `docs\` in the zip carries both READMEs, the GPL-3.0 license, `EXCEPTIONS.md` and `THIRD_PARTY_NOTICES.txt`.
- **Verifiable builds.** GitHub Actions builds the release. The zip and `ac-dlssg.dll` have build provenance attestations, and the release notes list their SHA-256.

### Requirements

- Assetto Corsa from Steam with Custom Shaders Patch 0.3.0 or newer, with CSP's DLSS upscaler turned on (any quality mode, including DLAA).
- An NVIDIA RTX 40 or RTX 50 GPU, or an RTX 30 GPU with dlssg_for_sm86, which needs driver R580 or newer. RTX 20 is not supported.
- Windows 10 version 2004 or newer, or Windows 11, with hardware-accelerated GPU scheduling on. On laptops, `acs.exe` must be set to "High performance" in the Windows graphics settings.
- Internet on the first install, for Streamline (about 276 MB) and, on an RTX 30, dlssg_for_sm86 (about 30 MB).

### Known limitations

- No HUD-less colour buffer: CSP app windows and on-screen UI can show small artifacts during fast motion.
- Not supported: VR, triple screens, HDR output, MSAA, `OLD_SWAPCHAIN=1`, `EXCLUSIVE_FULLSCREEN=1` and letterboxed output (a `video.ini` aspect ratio that differs from the window).
- Other CSP upscalers (FSR, XeSS) do not provide the inputs frame generation needs.
- No multi frame generation above 4X and no dynamic multi frame generation.
- CSP's FPS counter shows the real frames only; the NVIDIA overlay (Alt+R), RTSS or another external counter shows the frames on screen.
- On a 4 GB card video memory is tight: if the game stutters, lower CSP's texture or shadow quality.
- dlssg_for_sm86 has no LICENSE file, embeds NVIDIA's `nvngx_dlssg.dll`, and using it on RTX 30 circumvents a technical limitation that section 4.d of the NVIDIA RTX SDKs License forbids for that license's licensees. Whether to use it is the player's decision.

### Tested on

- **Desktop, ReShade mode:** Intel Core i9-9900, GeForce RTX 3080 10 GB (driver 616.64), 16 GB DDR4-3600, Windows 10 22H2, CSP 0.3.0-preview622, ReShade 6.8.0, dlssg_for_sm86 0.3.5. DLSS-G 2X doubled the frame rate with no ghosting and no blur, and a 15-minute session had no crash or freeze.
- **Laptop, standalone mode:** Acer Nitro 5 AN515-57, GeForce RTX 3050 Ti Laptop GPU 4 GB (driver 610.88), Core i5-11400H, 32 GB RAM, Windows 11 25H2, Optimus (the Intel iGPU drives the panel), no ReShade, CSP preview634, dlssg_for_sm86 0.3.5. DLSS-G 2X turned 31 real fps into 60 on screen, and 30 into 57 in a tunnel.
- 3X and 4X are covered by the unit tests and the test app; on the RTX 3080, dlssg_for_sm86 ran 3X and 4X in a native DLSS-G game before this project started.

[![English](https://img.shields.io/badge/lang-English-blue)](INSTALL.md) [![Русский](https://img.shields.io/badge/lang-Русский-red)](INSTALL.ru.md)

# How to install ac-dlssg

It takes about 5 minutes.

## What you need

- An **NVIDIA RTX 30, 40 or 50** graphics card, or an **RTX 2050** (an Ampere chip like the RTX 30 series). Laptops work too.
- **RTX 20** (2060, 2070, 2080, also Super, Ti and laptop GPUs) works too: the installer sets it up like an RTX 30, and a tester confirmed 2X, 3X and 4X on an RTX 2070 SUPER. GTX 16, MX and T-series cards are not supported: they have no DLSS.
- **Assetto Corsa** with a **Custom Shaders Patch (CSP) preview build** (0.3.0-preview or newer). You get it from CSP's author on [Patreon](https://www.patreon.com/c/x4fab/home).
- An internet connection. The installer downloads NVIDIA's files by itself.
- **RTX 40:** NVIDIA allows only 2X there. On a PC with RTX 40 GPUs only, the installer also downloads the third-party MFG-Unlocker for 3X and 4X (unofficial); the README's section "RTX 40 and MFG-Unlocker" explains it. `install.bat -NoMfgUnlocker` installs without it.

## Install

1. **Close the game.**
2. **Download** `ac-dlssg-1.2.0.zip` from the [latest release](https://github.com/flopsy2iqq/ac-dlssg/releases/latest).
3. **Unpack it.** Right-click the zip and choose **Extract All**.
4. **Double-click `install.bat`.**
   - If Windows asks "Do you want to allow this app to make changes?", click **Yes**.
   - **RTX 40 or RTX 50:** the installer also turns NVIDIA Smooth Motion off for Assetto Corsa (the driver's own frame generation, which frame generation cannot run on top of). It says so first, then Windows asks for administrator rights for this one change ("ac-dlssg driver profile helper"): click **Yes**. The uninstaller puts the setting back. To leave the driver profile alone, run `install.bat -KeepSmoothMotion` instead; the README's section "NVIDIA Smooth Motion" explains it. If the game is not in a folder named `assettocorsa` (every Steam install is), the driver may give `acs.exe` another game's profile; the installer then leaves it alone, and its warning says what to do by hand.
   - Wait until the window says it is done, then press **Enter**.
   - Done. You can now delete the unpacked folder and the zip. Everything the mod needs, its uninstaller too, is in the game folder.
5. **Turn on DLSS before you start the game.** Frame generation needs it.
   - In Content Manager, open Settings, then Custom Shaders Patch, and click **ADJUSTMENTS** (under Graphics) in the list on the left.
   - Under **Upscaling**, tick **Active** and set **Method** to **NVIDIA DLSS**. **EXTRA FX** (also under Graphics) must be active too.
   - Turn **MSAA off**, in every DLSS mode: in Content Manager, open Settings, then Assetto Corsa, then Video, and set **Anti-aliasing** (MSAA) to **Off**. With MSAA on, frame generation stays off.
   - **Using DLAA** (Quality: **DLAA (100%)**)? Then also untick **FXAA** there, or moving things leave a ghost trail. Keep **Enable post-processing effects** ticked, DLSS needs it.
6. **Start the game** as usual and drive. Frame generation turns on by itself.

## Check that it works

- Press **Ctrl+F10** while driving. Frame generation switches off, and pressing it again switches it back on.
- Open the **AC DLSS-G** window from CSP's app bar: in the game, move the mouse to the right edge of the screen, the app bar slides out, and click the AC DLSS-G logo. The window opens by itself the first time. It shows whether frame generation is on and your FPS, and it has the settings.
- CSP's own FPS counter shows only the real frames. The NVIDIA overlay (Alt+R) shows the frames on screen.

## Something is wrong?

- **No FPS gain:** check that DLSS is on (step 5), then open the **AC DLSS-G** window. It says why frame generation is off.
- **Ghosting** (a trail behind moving things) **with DLAA:** untick FXAA (step 5).
- **Ghosting at night or behind lights:** set CSP's DLSS preset to **M** (Content Manager: Settings, Custom Shaders Patch, ADJUSTMENTS, Upscaling, DLSS preset). The tested setup uses M.
- **The window says "NVIDIA Smooth Motion ... is active"** (RTX 40 and RTX 50): the NVIDIA driver's own frame generation took over the game, even if the NVIDIA App shows it off. Run `install.bat` again and click **Yes** when Windows asks: it turns Smooth Motion off for Assetto Corsa. Then restart the game. By hand: in [NVIDIA Profile Inspector](https://github.com/Orbmu2k/nvidiaProfileInspector), choose the profile **Assetto Corsa**, set **Smooth Motion - Enabled APIs** (`0xB0CC0875`) to **5**, click **Apply changes** and restart the game. If it still says so, set the same in the global profile (**_GLOBAL_DRIVER_PROFILE**). See the README for details.
- **VR** is not supported and not planned: ac-dlssg switches itself off in VR. Use SteamVR Motion Smoothing, Meta ASW or your OpenXR runtime's reprojection instead.
- **Neural rendering:** CSP's built-in Neural Rendering works together with ac-dlssg. The ReShade add-ons DLSS 5 Bridge and RenoDX DLSS5 cannot run together with ac-dlssg yet: use CSP's built-in Neural Rendering instead, or remove the add-on (or untick it in ReShade's Add-ons tab) for frame generation.
- **Streamline warnings** in the logs, such as `Invalid backbuffer resource extent` once at start or `Out of order frame` after frame generation switches off, are harmless.
- **Stutter on a G-Sync or FreeSync monitor:** in NVIDIA Control Panel, open Manage 3D settings > Program settings > Assetto Corsa (`acs.exe`): **Vertical sync = On**, and keep G-Sync on. It matters when the frames on screen are above your monitor's refresh rate.
- **On a laptop:** in Windows Settings, open System, then Display, then Graphics, and set `acs.exe` to **High performance**.
- **The game does not start:** first double-click **`collect-logs.bat`** in `ac-dlssg` (see the next point) and keep the zip it makes. Then uninstall the mod (see [Uninstall](#uninstall)). The game is back to how it was, and the zip stays in `ac-dlssg`.
- **Ask for help:** close the game, open the game folder (see [Uninstall](#uninstall)), open `ac-dlssg` and double-click **`collect-logs.bat`**. Attach the zip it makes next to it to a [new issue](https://github.com/flopsy2iqq/ac-dlssg/issues/new).

## Update

Download the new zip, unpack it and double-click its `install.bat`, as above. It replaces the old version by itself. It does not download NVIDIA's files again while the ones in the game folder are intact.

## Uninstall

1. **Open the game folder.** In Steam, right-click **Assetto Corsa**, choose **Manage**, then **Browse local files**.
2. **Open the `ac-dlssg` folder** and double-click **`uninstall.bat`**.
3. Wait until the window says it is done, then press **Enter**. The uninstaller removes itself too. If the installer turned NVIDIA Smooth Motion off, the uninstaller puts the setting back, and Windows asks for administrator rights again.

Your settings and logs stay in `ac-dlssg`. After the uninstall you can simply delete the `ac-dlssg` folder.

Still on version 1.0.0? It has no `uninstall.bat` in `ac-dlssg`: double-click `tools\uninstall.bat` in its unpacked folder, or install this version first.

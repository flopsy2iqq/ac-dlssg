[![English](https://img.shields.io/badge/lang-English-blue)](INSTALL.md) [![Русский](https://img.shields.io/badge/lang-Русский-red)](INSTALL.ru.md)

# How to install ac-dlssg

It takes about 5 minutes.

## What you need

- An **NVIDIA RTX 30, 40 or 50** graphics card. Laptops work too.
- **Assetto Corsa** with a **Custom Shaders Patch (CSP) preview build** (0.3.0-preview or newer). You get it from CSP's author on [Patreon](https://www.patreon.com/c/x4fab/home).
- An internet connection. The installer downloads NVIDIA's files by itself.

## Install

1. **Close the game.**
2. **Download** `ac-dlssg-1.0.0.zip` from the [latest release](https://github.com/flopsy2iqq/ac-dlssg/releases/latest).
3. **Unpack it.** Right-click the zip and choose **Extract All**.
4. **Double-click `install.bat`.**
   - If Windows asks "Do you want to allow this app to make changes?", click **Yes**.
   - Wait until the window says it is done, then press **Enter**.
   - Done. You can now delete the unpacked folder and the zip. Everything the mod needs, its uninstaller too, is in the game folder.
5. **Turn on DLSS before you start the game.** Frame generation needs it.
   - In Content Manager, open Settings, then Custom Shaders Patch, and click **ADJUSTMENTS** (under Graphics) in the list on the left.
   - Under **Upscaling**, tick **Active** and set **Method** to **NVIDIA DLSS**. **EXTRA FX** (also under Graphics) must be active too.
   - **Using DLAA** (Quality: **DLAA (100%)**)? Then also turn off MSAA and FXAA, or moving things leave a ghost trail. In Content Manager, open Settings, then Assetto Corsa, then Video, set **MSAA** to **Off** and untick **FXAA**. Keep **Enable post-processing effects** ticked, DLSS needs it.
6. **Start the game** as usual and drive. Frame generation turns on by itself.

## Check that it works

- Press **Ctrl+F10** while driving. Frame generation switches off, and pressing it again switches it back on.
- Open the **AC DLSS-G** window from CSP's app bar. It shows whether frame generation is on and your FPS, and it has the settings.
- CSP's own FPS counter shows only the real frames. The NVIDIA overlay (Alt+R) shows the frames on screen.

## Something is wrong?

- **No FPS gain:** check that DLSS is on (step 5), then open the **AC DLSS-G** window. It says why frame generation is off.
- **Ghosting** (a trail behind moving things) **with DLAA:** turn off MSAA and FXAA (step 5).
- **On a laptop:** in Windows Settings, open System, then Display, then Graphics, and set `acs.exe` to **High performance**.
- **The game does not start:** first double-click **`collect-logs.bat`** in `ac-dlssg` (see the next point) and keep the zip it makes. Then uninstall the mod (see [Uninstall](#uninstall)). The game is back to how it was, and the zip stays in `ac-dlssg`.
- **Ask for help:** close the game, open the game folder (see [Uninstall](#uninstall)), open `ac-dlssg` and double-click **`collect-logs.bat`**. Attach the zip it makes next to it to a [new issue](https://github.com/flopsy2iqq/ac-dlssg/issues/new).

## Update

Download the new zip, unpack it and double-click its `install.bat`, as above. It replaces the old version by itself. It does not download NVIDIA's files again while the ones in the game folder are intact.

## Uninstall

1. **Open the game folder.** In Steam, right-click **Assetto Corsa**, choose **Manage**, then **Browse local files**.
2. **Open the `ac-dlssg` folder** and double-click **`uninstall.bat`**.
3. Wait until the window says it is done, then press **Enter**. The uninstaller removes itself too.

Your settings and logs stay in `ac-dlssg`. To delete them as well, run `uninstall.bat -RemoveData` from a command prompt in that folder.

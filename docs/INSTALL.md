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
5. **Start the game** as usual and drive. Frame generation turns on by itself.

## Check that it works

- In CSP's graphics settings, **DLSS** must be the upscaler. In Content Manager, open Settings, then Custom Shaders Patch, and type `DLSS` in the search box.
- Press **Ctrl+F10** while driving. Frame generation switches off, and pressing it again switches it back on.
- Open the **AC DLSS-G** window from CSP's app bar. It shows whether frame generation is on and your FPS, and it has the settings.
- CSP's own FPS counter shows only the real frames. The NVIDIA overlay (Alt+R) shows the frames on screen.

## Something is wrong?

- **No FPS gain:** open the **AC DLSS-G** window. It says why frame generation is off.
- **On a laptop:** in Windows Settings, open System, then Display, then Graphics, and set `acs.exe` to **High performance**.
- **The game does not start:** in the unpacked folder, open `tools` and double-click **`uninstall.bat`**. The game is back to how it was.
- **Ask for help:** close the game, double-click `tools\collect-logs.bat`, and attach the zip it makes to a [new issue](https://github.com/flopsy2iqq/ac-dlssg/issues/new).

## Uninstall

Double-click `tools\uninstall.bat` in the unpacked folder.

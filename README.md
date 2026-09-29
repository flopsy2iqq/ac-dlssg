# ac-dlssg

NVIDIA DLSS Frame Generation (DLSS-G 2X) for **Assetto Corsa** with **Custom Shaders Patch**.

**Status: test build. Frame generation works in game.** On the test PC the frame rate doubles with no ghosting and no blur. The pictures are in [Screenshots](#screenshots).

**[Инструкция по установке на русском](#по-русски)**

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
![DLSS-G 2X, chase view: 57 real fps, 112 fps on screen](docs/screenshots/fg2x-2.jpg)
![DLSS-G 2X, night: 53 real fps, 105 fps on screen](docs/screenshots/fg2x-3.jpg)
![DLSS-G 2X, side view at sunset: 58 real fps, 116 fps on screen](docs/screenshots/fg2x-4.jpg)
![DLSS-G 2X, cockpit in rain: 46 real fps, 90 fps on screen](docs/screenshots/fg2x-5.jpg)
![DLSS-G 2X, front view: 55 real fps, 108 fps on screen](docs/screenshots/fg2x-6.jpg)
![DLSS-G 2X, night with CSP's render stats: 59 real fps, 119 fps on screen](docs/screenshots/fg2x-7.jpg)

## Requirements

- **Assetto Corsa** from Steam with **Custom Shaders Patch 0.3.0** or newer. It was tested with preview622 and preview634.
- **CSP's DLSS upscaling turned on**, in any quality mode including DLAA. Frame generation takes its depth and motion vectors from it.
- **An NVIDIA RTX GPU:**
  - RTX 40 and RTX 50 support DLSS-G themselves.
  - RTX 30, including laptop GPUs such as the RTX 3050 Ti, needs the third-party [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86). It is not part of this project; install it yourself from its page, and see [RTX 30 and dlssg_for_sm86](#rtx-30-and-dlssg_for_sm86). It needs NVIDIA driver R580 or newer.
  - RTX 20 is not supported by this project.
- **Windows 10 version 2004 or newer, or Windows 11.** Hardware-accelerated GPU scheduling must be on: Settings > System > Display > Graphics > Change default graphics settings.
- **Laptops:** set `acs.exe` to "High performance" in the same Graphics settings, so the game runs on the NVIDIA GPU.
- **Internet on the first install.** The installer downloads NVIDIA Streamline 2.14.1 (about 276 MB) from NVIDIA's GitHub release and checks its hash and NVIDIA's signatures. This project never ships NVIDIA files.
- **ReShade is optional.** With ReShade 6.8+ (add-on build) installed as `dxgi.dll`, the bridge loads through ReShade. Without it, the bridge itself is installed as `dxgi.dll`. The installer picks the mode.

## Install

1. Close Assetto Corsa and Content Manager's game session.
2. **RTX 30 only:** install dlssg_for_sm86 as its page describes: `version.dll` and `dlssg_sm86.ini` go next to `acs.exe`.
3. Download `ac-dlssg-<version>-test.zip` from [Releases](https://github.com/flopsy2iqq/ac-dlssg/releases) and extract it anywhere.
4. Right-click `install.ps1` and choose **Run with PowerShell**. If that does not start it, open PowerShell in the folder and run:
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```
   The script finds the game through Steam. It then downloads Streamline and shows NVIDIA's license files; type `y` to accept them. If the game is under `C:\Program Files (x86)` and the script reports "access denied", run the same command from PowerShell opened as administrator.
5. Start the game as usual and make sure DLSS is the upscaler in CSP's graphics settings.
6. Drive. Frame generation turns on by itself once CSP's DLSS runs; it stays off in the pause menu and the main menu.

## Use

- **Ctrl+F10** switches frame generation on and off, for an A/B comparison of the same scene.
- CSP's own FPS counter shows real frames. Use the NVIDIA overlay (Alt+R), RTSS or another external counter to see the frames on screen.
- Settings are in `<game>\ac-dlssg\ac-dlssg.ini`: `start_with_fg`, `hotkey` and `log_level`.
- An in-game settings window is coming in the next build.

## Uninstall

Run `uninstall.ps1` from the same folder. It restores every file the installer changed, and removes the bridge, the Streamline files and the Lua app. Logs and settings stay in `<game>\ac-dlssg` unless you add `-RemoveData`.

## If something goes wrong

- **The game starts but the frame rate does not change.** Look at `<game>\ac-dlssg\logs\bridge.log`. The line `fg: DLSS-G on` means frame generation runs. A line `fg: frame without DLSS-G: <reason>`, or `DLSS-G is not supported on this adapter (...)`, says why it does not.
- **The game crashes or does not start.** Run `uninstall.ps1`; the game is then back to how it was.
- **Reporting a problem:** run `collect-logs.ps1` from the package after the game is closed. It only reads files and writes one zip next to itself. Attach that zip to a [GitHub issue](https://github.com/flopsy2iqq/ac-dlssg/issues).

## Known limitations

- 2X only: one generated frame per real frame.
- CSP app windows and on-screen UI can show small artifacts during fast motion. A HUD-less colour buffer is planned.
- The following are not supported: VR, triple screens, HDR output, MSAA, `EXCLUSIVE_FULLSCREEN=1`, and letterboxed output.
- Other CSP upscalers (FSR, XeSS) do not provide the inputs frame generation needs.

## RTX 30 and dlssg_for_sm86

NVIDIA locks DLSS Frame Generation to RTX 40 and newer. [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) by sdli1995, which includes Coldwood1026's RTX 20 work, lifts that lock on RTX 30. The author agreed to this project pointing to it.

- ac-dlssg does not contain, download or modify it.
- That repository has no LICENSE file. Its README says the source is GPLv3, but no source is published.
- It embeds NVIDIA's `nvngx_dlssg.dll`, which is not relicensed.
- Using it on RTX 30 circumvents a technical limitation, which section 4.d of the NVIDIA RTX SDKs License forbids for that license's licensees.

Whether to use it is your decision.

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

- [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) by sdli1995, with Coldwood1026's work: DLSS-G on RTX 30.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline): the SDK that delivers DLSS Frame Generation and Reflex. Its DLLs are downloaded from NVIDIA's release on your PC.
- [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (MIT): the approach for hooking CSP's DLSS call.
- [open-shaders](https://github.com/alandtse/open-shaders) and [Community Shaders](https://github.com/doodlum/skyrim-community-shaders): the DirectX 11 game plus DirectX 12 Streamline swap chain design.

## License

GPL-3.0. See [LICENSE](LICENSE).

---

## По-русски

Генерация кадров NVIDIA DLSS Frame Generation (DLSS-G 2X) для **Assetto Corsa** с **Custom Shaders Patch**.

**Статус: тестовая сборка, генерация кадров в игре работает.** На тестовом ПК FPS вырос вдвое, без гостинга и мыла. Скриншоты выше: слева вверху счётчик CSP, это **настоящие** кадры; справа вверху оверлей NVIDIA, это кадры **на экране после генерации**. Тестовый ПК: Intel Core i9-9900 (8 ядер, 16 потоков), RTX 3080 10 ГБ (драйвер 616.64), 16 ГБ DDR4-3600, Windows 10 22H2, CSP 0.3.0-preview622, dlssg_for_sm86 0.3.5.

### Что нужно

- **Assetto Corsa** из Steam с **Custom Shaders Patch 0.3.0** или новее. Проверено на preview622 и preview634.
- **DLSS-апскейл в CSP включён**, в любом режиме, включая DLAA. Генерация берёт из него глубину и векторы движения.
- **Видеокарта NVIDIA RTX:**
  - RTX 40 и RTX 50 поддерживают DLSS-G сами.
  - RTX 30, в том числе ноутбучные вроде 3050 Ti, требует сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86). Он не входит в этот проект, ставите его сами по инструкции с его страницы. Нужен драйвер NVIDIA R580 или новее.
  - RTX 20 этот проект не поддерживает.
- **Windows 10 2004+ или Windows 11.** Аппаратное планирование GPU должно быть включено: Параметры > Система > Дисплей > Графика > Изменить стандартные параметры графики.
- **Ноутбук:** там же, в настройках графики, поставьте `acs.exe` в «Высокая производительность», чтобы игра шла на NVIDIA.
- **Интернет при первой установке.** Установщик скачивает NVIDIA Streamline 2.14.1 (около 276 МБ) с GitHub NVIDIA и проверяет хеш и подписи. Файлов NVIDIA в проекте нет.
- **ReShade не обязателен.** Если стоит ReShade 6.8+ (версия с аддонами), мост грузится через него. Если нет, мост ставится как `dxgi.dll` сам. Установщик выбирает режим сам.

### Установка

1. Закройте Assetto Corsa.
2. **Только RTX 30:** поставьте dlssg_for_sm86 по инструкции с его страницы: `version.dll` и `dlssg_sm86.ini` кладутся рядом с `acs.exe`.
3. Скачайте `ac-dlssg-<версия>-test.zip` из [Releases](https://github.com/flopsy2iqq/ac-dlssg/releases) и распакуйте в любую папку.
4. Щёлкните правой кнопкой по `install.ps1` и выберите **«Выполнить с помощью PowerShell»**. Если не запускается, откройте PowerShell в этой папке и выполните:
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```
   Скрипт сам найдёт игру через Steam и скачает Streamline. Он покажет лицензии NVIDIA, введите `y`. Если игра стоит в `C:\Program Files (x86)` и скрипт пишет «доступ запрещён», выполните ту же команду в PowerShell, открытом от имени администратора.
5. Запустите игру как обычно. В графических настройках CSP апскейлером должен быть DLSS.
6. Выезжайте. Генерация включается сама, как только заработает DLSS в CSP. В паузе и в меню она выключена.

### Как пользоваться

- **Ctrl+F10** включает и выключает генерацию, так удобно сравнить одну и ту же сцену.
- Счётчик FPS в CSP показывает настоящие кадры. Чтобы видеть кадры на экране, нужен внешний счётчик: оверлей NVIDIA (Alt+R), RTSS и т. п.
- Настройки лежат в `<игра>\ac-dlssg\ac-dlssg.ini`: `start_with_fg`, `hotkey`, `log_level`.
- Окно настроек прямо в игре будет в следующей сборке.

### Удаление

Запустите `uninstall.ps1` из той же папки. Он вернёт всё, что поменял установщик, и уберёт мост, Streamline и Lua-приложение. Логи и настройки остаются в `<игра>\ac-dlssg`. Чтобы удалить и их, добавьте `-RemoveData`.

### Если что-то не так

- **Игра запускается, но FPS не меняется.** Посмотрите `<игра>\ac-dlssg\logs\bridge.log`. Строка `fg: DLSS-G on` значит, что генерация работает. Строки `fg: frame without DLSS-G: <причина>` или `DLSS-G is not supported on this adapter (...)` объясняют, почему нет.
- **Игра вылетает или не запускается.** Запустите `uninstall.ps1`, игра вернётся в прежнее состояние.
- **Сообщить о проблеме:** после закрытия игры запустите `collect-logs.ps1`. Он только читает файлы и кладёт рядом с собой один zip. Приложите этот zip к [issue на GitHub](https://github.com/flopsy2iqq/ac-dlssg/issues).

### Ограничения

- Только 2X: один сгенерированный кадр на один настоящий.
- Окна приложений CSP и интерфейс на экране при быстром движении могут давать мелкие артефакты. Буфер без интерфейса запланирован.
- Не поддерживаются: VR, тройные мониторы, HDR, MSAA, `EXCLUSIVE_FULLSCREEN=1`, вывод с чёрными полосами.

### Про dlssg_for_sm86

NVIDIA разрешает DLSS-G только на RTX 40 и новее. Сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) от sdli1995 (с работой Coldwood1026) снимает это ограничение на RTX 30. Автор согласен, что этот проект ссылается на него.

- ac-dlssg его не содержит, не скачивает и не меняет.
- У того репозитория нет файла LICENSE. В README сказано, что исходники под GPLv3, но они не опубликованы.
- Он содержит `nvngx_dlssg.dll` от NVIDIA, лицензия на который не менялась.
- Его использование на RTX 30 обходит техническое ограничение, что запрещает пункт 4.d лицензии NVIDIA RTX SDKs для её лицензиатов.

Ставить его или нет, решаете вы.

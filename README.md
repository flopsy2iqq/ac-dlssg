# ac-dlssg

**Status: M1 (DirectX 12 presentation without frame generation) verified in game; nothing to install for players yet.**

NVIDIA DLSS Frame Generation (DLSS-G) for Assetto Corsa with Custom Shaders Patch.

Assetto Corsa renders with DirectX 11, while DLSS Frame Generation runs only on DirectX 12 and Vulkan, through NVIDIA Streamline. This project bridges the gap: the game keeps rendering in DirectX 11, the finished frame is presented through a DirectX 12 swap chain created with Streamline, and DLSS-G inserts the generated frames there. Depth and motion vectors come from the DLSS upscaling pass that CSP already runs.

## Planned requirements

- Assetto Corsa with Custom Shaders Patch 0.3.0 or newer, with CSP's DLSS upscaler enabled.
- An NVIDIA RTX 40 or RTX 50 GPU. On RTX 20 and RTX 30 cards NVIDIA locks DLSS-G. The third-party [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) project lifts that lock. It is not part of this project, and you use it at your own risk.
- Windows 10 version 2004 (build 19041) or newer, with hardware-accelerated GPU scheduling turned on.

## Prior art

- [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (MIT) mirrors a DirectX 11 game's DLSS inputs onto a private DirectX 12 device.
- [Community Shaders](https://github.com/doodlum/skyrim-community-shaders) runs frame generation in DirectX 11 Skyrim through a DirectX 12 swap chain proxy.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) is the SDK that delivers DLSS Frame Generation.

## License

GPL-3.0. See [LICENSE](LICENSE).

---

## По-русски

**Статус: M1 (вывод кадра через DirectX 12, без генерации кадров) проверен в игре; игрокам ставить пока нечего.**

Генерация кадров NVIDIA (DLSS-G) для Assetto Corsa с Custom Shaders Patch. Игра рисует в DirectX 11, а генерация NVIDIA работает только в DirectX 12. Мост выводит готовый кадр через DirectX 12 и Streamline. Глубина и векторы движения берутся из DLSS-апскейла CSP.

На RTX 40 и RTX 50 всё должно работать штатно. На RTX 20 и RTX 30 генерацию можно открыть только сторонним [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86), на свой риск; в этот проект он не входит.

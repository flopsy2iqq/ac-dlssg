[![English](https://img.shields.io/badge/lang-English-blue)](README.md) [![Русский](https://img.shields.io/badge/lang-Русский-red)](README.ru.md)

# ac-dlssg

Генерация кадров NVIDIA DLSS Frame Generation (DLSS-G 2X) для **Assetto Corsa** с **Custom Shaders Patch**.

**Статус: тестовая сборка, генерация кадров в игре работает.** На тестовом ПК FPS вырос вдвое, без гостинга и мыла. Картинки в разделе [Скриншоты](#скриншоты).

Assetto Corsa рисует через DirectX 11. DLSS Frame Generation работает только в DirectX 12 и Vulkan, через NVIDIA Streamline. ac-dlssg соединяет одно с другим:

- Игра по-прежнему рисует через DirectX 11.
- Каждый готовый кадр выводится через цепочку обмена DirectX 12, созданную через Streamline, и DLSS-G вставляет сгенерированный кадр между каждыми двумя настоящими.
- Глубину и векторы движения мост берёт из DLSS-апскейла, который CSP и так выполняет.
- Камеру передаёт маленькое Lua-приложение для CSP, которое ставит установщик.

## Скриншоты

DLSS-G 2X в Assetto Corsa с CSP:

- **Слева вверху:** счётчик FPS в CSP. Он показывает **настоящие** кадры, которые рисует игра.
- **Справа вверху:** оверлей NVIDIA. Он показывает кадры **на экране после генерации**.

Тестовый ПК: Intel Core i9-9900 (8 ядер, 16 потоков), GeForce RTX 3080 10 ГБ (драйвер 616.64), 16 ГБ DDR4-3600, Windows 10 22H2, CSP 0.3.0-preview622, dlssg_for_sm86 0.3.5.

![DLSS-G 2X, вид из кокпита: 62 настоящих fps, 124 fps на экране](docs/screenshots/fg2x-1.jpg)
![DLSS-G 2X, вид сзади: 55 настоящих fps, 112 fps на экране](docs/screenshots/fg2x-2.jpg)
![DLSS-G 2X, ночь: 53 настоящих fps, 105 fps на экране](docs/screenshots/fg2x-3.jpg)
![DLSS-G 2X, вид сбоку на закате: 58 настоящих fps, 116 fps на экране](docs/screenshots/fg2x-4.jpg)
![DLSS-G 2X, кокпит в дождь: 46 настоящих fps, 90 fps на экране](docs/screenshots/fg2x-5.jpg)
![DLSS-G 2X, вид спереди: 55 настоящих fps, 108 fps на экране](docs/screenshots/fg2x-6.jpg)
![DLSS-G 2X, ночь со статистикой рендера CSP: 59 настоящих fps, 119 fps на экране](docs/screenshots/fg2x-7.jpg)

### Тест на ноутбуке

Ноутбук друга Acer Nitro 5 AN515-57: GeForce RTX 3050 Ti Laptop GPU (4 ГБ, драйвер 610.88), Core i5-11400H, 32 ГБ ОЗУ, Windows 11 25H2, Optimus (экран подключён к встроенной графике Intel), без ReShade (режим без ReShade), CSP preview634. 31 настоящий fps превращается в 60 на экране.

![DLSS-G 2X на ноутбуке с RTX 3050 Ti: 31 настоящий fps, 60 fps на экране](docs/screenshots/fg2x-laptop-1.jpg)
![DLSS-G 2X на ноутбуке с RTX 3050 Ti в тоннеле: 30 настоящих fps, 57 fps на экране](docs/screenshots/fg2x-laptop-2.jpg)

На карте с 4 ГБ видеопамяти мало: если игра подтормаживает, снизьте в CSP качество текстур или теней.

## Что нужно

- **Assetto Corsa** из Steam с **Custom Shaders Patch 0.3.0** или новее. Проверено на preview622 и preview634.
- **DLSS-апскейл в CSP включён**, в любом режиме, включая DLAA. Генерация берёт из него глубину и векторы движения.
- **Видеокарта NVIDIA RTX:**
  - RTX 40 и RTX 50 поддерживают DLSS-G сами.
  - RTX 30, в том числе ноутбучные вроде 3050 Ti, требует сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86). Он не входит в этот проект, ставите его сами по инструкции с его страницы, см. [Про dlssg_for_sm86](#про-dlssg_for_sm86). Нужен драйвер NVIDIA R580 или новее.
  - RTX 20 этот проект не поддерживает.
- **Windows 10 2004+ или Windows 11.** Аппаратное планирование GPU должно быть включено: Параметры > Система > Дисплей > Графика > Изменить стандартные параметры графики.
- **Ноутбук:** там же, в настройках графики, поставьте `acs.exe` в «Высокая производительность», чтобы игра шла на NVIDIA.
- **Интернет при первой установке.** Установщик скачивает NVIDIA Streamline 2.14.1 (около 276 МБ) с GitHub NVIDIA и проверяет хеш и подписи NVIDIA. Файлов NVIDIA в проекте нет.
- **ReShade не обязателен.** Если стоит ReShade 6.8+ (версия с аддонами) как `dxgi.dll`, мост грузится через него. Если нет, мост сам ставится как `dxgi.dll`. Установщик выбирает режим сам.

## Установка

1. Закройте Assetto Corsa и игровую сессию Content Manager.
2. **Только RTX 30:** поставьте dlssg_for_sm86 по инструкции с его страницы: `version.dll` и `dlssg_sm86.ini` кладутся рядом с `acs.exe`.
3. Скачайте `ac-dlssg-<версия>-test.zip` из [Releases](https://github.com/flopsy2iqq/ac-dlssg/releases) и распакуйте в любую папку.
4. Щёлкните правой кнопкой по `install.ps1` и выберите **«Выполнить с помощью PowerShell»**. Если не запускается, откройте PowerShell в этой папке и выполните:
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```
   Скрипт сам найдёт игру через Steam и скачает Streamline. Он покажет лицензии NVIDIA, введите `y`. Если игра стоит в `C:\Program Files (x86)` и скрипт пишет «доступ запрещён», выполните ту же команду в PowerShell, открытом от имени администратора.
5. Запустите игру как обычно. В графических настройках CSP апскейлером должен быть DLSS.
6. Выезжайте. Генерация включается сама, как только заработает DLSS в CSP. В паузе и в меню она выключена.

## Как пользоваться

- **Ctrl+F10** включает и выключает генерацию, так удобно сравнить одну и ту же сцену.
- Счётчик FPS в CSP показывает настоящие кадры. Чтобы видеть кадры на экране, нужен внешний счётчик: оверлей NVIDIA (Alt+R), RTSS и т. п.
- Настройки лежат в `<игра>\ac-dlssg\ac-dlssg.ini`: `start_with_fg`, `hotkey`, `log_level`.
- Окно настроек прямо в игре будет в следующей сборке.

## Удаление

Запустите `uninstall.ps1` из той же папки. Он вернёт всё, что поменял установщик, и уберёт мост, Streamline и Lua-приложение. Логи и настройки остаются в `<игра>\ac-dlssg`. Чтобы удалить и их, добавьте `-RemoveData`.

## Если что-то не так

- **Игра запускается, но FPS не меняется.** Посмотрите `<игра>\ac-dlssg\logs\bridge.log`. Строка `fg: DLSS-G on` значит, что генерация работает. Строки `fg: frame without DLSS-G: <причина>` или `DLSS-G is not supported on this adapter (...)` объясняют, почему нет.
- **Игра вылетает или не запускается.** Запустите `uninstall.ps1`, игра вернётся в прежнее состояние.
- **Сообщить о проблеме:** после закрытия игры запустите `collect-logs.ps1`. Он только читает файлы и кладёт рядом с собой один zip. Приложите этот zip к [issue на GitHub](https://github.com/flopsy2iqq/ac-dlssg/issues).

## Ограничения

- Только 2X: один сгенерированный кадр на один настоящий.
- Окна приложений CSP и интерфейс на экране при быстром движении могут давать мелкие артефакты. Буфер без интерфейса запланирован.
- Не поддерживаются: VR, тройные мониторы, HDR, MSAA, `EXCLUSIVE_FULLSCREEN=1`, вывод с чёрными полосами.
- Другие апскейлеры CSP (FSR, XeSS) не дают данных, которые нужны генерации кадров.

## Про dlssg_for_sm86

NVIDIA разрешает DLSS-G только на RTX 40 и новее. Сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) от sdli1995 (с работой Coldwood1026 для RTX 20) снимает это ограничение на RTX 30. Автор согласен, что этот проект ссылается на него.

- ac-dlssg его не содержит, не скачивает и не меняет.
- У того репозитория нет файла LICENSE. В README сказано, что исходники под GPLv3, но они не опубликованы.
- Он содержит `nvngx_dlssg.dll` от NVIDIA, лицензия на который не менялась.
- Его использование на RTX 30 обходит техническое ограничение, что запрещает пункт 4.d лицензии NVIDIA RTX SDKs для её лицензиатов.

Ставить его или нет, решаете вы.

## Как это работает (кратко)

- `ac-dlssg.dll` (или `dxgi.dll` в режиме без ReShade) экспортирует функции DXGI и подменяет цепочку обмена DirectX 11, которую создаёт CSP, своим посредником.
- Каждый кадр через общую текстуру копируется в цепочку обмена DirectX 12, созданную через NVIDIA Streamline 2.14.1, с маркерами Reflex и PCL.
- Вызов DLSS в CSP перехватывается. Его глубина и векторы движения копируются в общие текстуры и помечаются для DLSS-G.
- Lua-приложение CSP `AcDlssg` пишет камеру в общую память. Мост собирает из неё константы камеры для Streamline.

Полное описание устройства в [docs/superpowers/specs](docs/superpowers/specs), записи по этапам в [docs/superpowers/plans](docs/superpowers/plans).

## Сборка из исходников

Нужны Visual Studio 2022 Build Tools (MSVC 14.44), Windows SDK 10.0.22621 и CMake.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\acdb_tests.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-test-package.ps1
```

`fetch-deps.ps1` кладёт закреплённые версии Streamline SDK и заголовков NVAPI в `deps\`, эта папка не попадает в git.

## Благодарности

- [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) от sdli1995, с работой Coldwood1026: DLSS-G на RTX 30.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline): SDK, через который работают DLSS Frame Generation и Reflex. Его DLL скачиваются с релиза NVIDIA на вашем ПК.
- [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (MIT): способ перехватить вызов DLSS в CSP.
- [open-shaders](https://github.com/alandtse/open-shaders) и [Community Shaders](https://github.com/doodlum/skyrim-community-shaders): схема «игра на DirectX 11 плюс цепочка обмена DirectX 12 через Streamline».

## Лицензия

GPL-3.0. См. [LICENSE](LICENSE).

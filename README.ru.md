<div align="center">

<img src="docs/logo/ac-dlssg-banner.png" alt="ac-dlssg: генерация кадров DLSS для Assetto Corsa" width="100%">

[![English](https://img.shields.io/badge/English-1F1F24?style=for-the-badge)](README.md) [![Русский](https://img.shields.io/badge/Русский-1F1F24?style=for-the-badge)](README.ru.md) [![Установка](https://img.shields.io/badge/Установка-E10600?style=for-the-badge&logo=windows&logoColor=white)](docs/INSTALL.ru.md) [![Поддержать на Ko-fi](https://img.shields.io/badge/Поддержать-Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flopsy2iq)

[![Release](https://img.shields.io/github/v/release/flopsy2iqq/ac-dlssg?color=E10600)](https://github.com/flopsy2iqq/ac-dlssg/releases/latest) [![Downloads](https://img.shields.io/github/downloads/flopsy2iqq/ac-dlssg/total?color=E10600)](https://github.com/flopsy2iqq/ac-dlssg/releases) [![Build](https://github.com/flopsy2iqq/ac-dlssg/actions/workflows/build.yml/badge.svg)](https://github.com/flopsy2iqq/ac-dlssg/actions/workflows/build.yml) ![NVIDIA RTX 30 | 40 | 50](https://img.shields.io/badge/NVIDIA-RTX%2030%20%7C%2040%20%7C%2050-76B900?logo=nvidia&logoColor=white) [![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

Генерация кадров NVIDIA DLSS Frame Generation (DLSS-G 2X, 3X и 4X) для **Assetto Corsa** с **Custom Shaders Patch**.

</div>

**Версия 1.0.0, генерация кадров в игре работает.** На тестовом ПК FPS вырос вдвое, без гостинга и мыла. Картинки в разделе [Скриншоты](#скриншоты).

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

### DLSS-G 4X

Тот же ПК с кнопкой **4X** в окне: примерно 40 настоящих кадров превращаются примерно в 160 на экране, без гостинга и статтеров.

![DLSS-G 4X, вид сзади под дождём: 41 настоящий кадр, 160 на экране](docs/screenshots/fg4x-1.jpg)
![DLSS-G 4X, тоннель: 40 настоящих, 159 на экране](docs/screenshots/fg4x-2.jpg)
![DLSS-G 4X, дождь на закате: 40 настоящих, 169 на экране](docs/screenshots/fg4x-3.jpg)
![DLSS-G 4X, из кокпита под дождём: 40 настоящих, 159 на экране](docs/screenshots/fg4x-4.jpg)

### Тест на ноутбуке

Ноутбук друга Acer Nitro 5 AN515-57: GeForce RTX 3050 Ti Laptop GPU (4 ГБ, драйвер 610.88), Core i5-11400H, 32 ГБ ОЗУ, Windows 11 25H2, Optimus (экран подключён к встроенной графике Intel), без ReShade (режим без ReShade), CSP preview634. 31 настоящий fps превращается в 60 на экране.

![DLSS-G 2X на ноутбуке с RTX 3050 Ti: 31 настоящий fps, 60 fps на экране](docs/screenshots/fg2x-laptop-1.jpg)
![DLSS-G 2X на ноутбуке с RTX 3050 Ti в тоннеле: 30 настоящих fps, 57 fps на экране](docs/screenshots/fg2x-laptop-2.jpg)

Тот же ноутбук на **4X**: 23 настоящих кадра превращаются в 89 на экране, и видеопамяти 4 ГБ на это хватило.

![DLSS-G 4X на ноутбуке с RTX 3050 Ti: 23 настоящих, 89 на экране](docs/screenshots/fg4x-laptop-1.jpg)

На карте с 4 ГБ видеопамяти мало. Мост подстраивается под это сам (`fg_vram_headroom_mib=auto`, см. [Как пользоваться](#как-пользоваться)): на такой карте он не держит лишнего запаса памяти и включает генерацию, даже если памяти немного не хватает, а окно тогда пишет "video memory is tight". Если игра подтормаживает, снизьте в CSP качество текстур или теней.

## Что нужно

- **Assetto Corsa** из Steam с **preview-версией Custom Shaders Patch** (0.3.0-preview или новее). Проверено на preview622 и preview634. Preview-версии CSP можно получить у автора CSP на [Patreon](https://www.patreon.com/c/x4fab/home).
- **DLSS-апскейл в CSP включён**, в любом режиме, включая DLAA. Генерация берёт из него глубину и векторы движения. С DLAA выключите ещё MSAA и FXAA в видеонастройках игры (Content Manager: Настройки, Assetto Corsa, Графика: Сглаживание «Выкл.», галочка «Быстрое сглаживание (FXAA)» снята, постобработка включена), иначе за движущимися объектами будет шлейф.
- **Видеокарта NVIDIA RTX:**
  - RTX 40 и RTX 50 поддерживают DLSS-G сами.
  - RTX 30, в том числе ноутбучные вроде 3050 Ti, требует сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86). Ставить его руками не нужно: на RTX 30 установщик сам скачивает закреплённую версию 0.3.5 из репозитория автора, проверяет её хеши и кладёт рядом с `acs.exe`. См. [Про dlssg_for_sm86](#про-dlssg_for_sm86). Нужен драйвер NVIDIA R580 или новее.
  - RTX 20 этот проект не поддерживает.
- **Windows 10 2004+ или Windows 11.** Аппаратное планирование GPU должно быть включено: Параметры > Система > Дисплей > Графика > Изменить стандартные параметры графики.
- **Ноутбук:** там же, в настройках графики, поставьте `acs.exe` в «Высокая производительность», чтобы игра шла на NVIDIA.
- **Интернет при первой установке.** Установщик скачивает NVIDIA Streamline 2.14.1 (около 276 МБ) с GitHub NVIDIA и проверяет хеш и подписи NVIDIA, а на RTX 30 ещё dlssg_for_sm86 (около 30 МБ). При обновлении он их заново не скачивает, пока установленные файлы целы. Ни файлов NVIDIA, ни dlssg_for_sm86 в проекте нет.
- **ReShade не обязателен.** Если стоит ReShade 6.8+ (версия с аддонами) как `dxgi.dll`, мост грузится через него. Если нет, мост сам ставится как `dxgi.dll`. Установщик выбирает режим сам.

## Установка

1. Закройте Assetto Corsa и игровую сессию Content Manager.
2. Скачайте `ac-dlssg-<версия>.zip` из [Releases](https://github.com/flopsy2iqq/ac-dlssg/releases) и распакуйте в любую папку. В ней лежит `install.bat` и папки `docs`, `files`, `scripts` и `tools`.
3. Дважды щёлкните `install.bat`. Это всё: установщик ничего не спрашивает. Он сам найдёт игру через Steam, скачает Streamline (на RTX 30 и dlssg_for_sm86), напишет, где лежат файлы лицензий NVIDIA (установка означает согласие с ними), и всё поставит. Если игра стоит в `C:\Program Files (x86)`, Windows спросит разрешение от имени администратора: подтвердите, и установка продолжится в новом окне. В конце окно ждёт Enter. После этого распакованную папку и архив можно удалить: программа удаления и сбор логов лежат в папке игры, в `ac-dlssg`.
4. Перед запуском игры включите DLSS в CSP: в Content Manager Настройки, Custom Shaders Patch, ADJUSTMENTS (группа Graphics), блок Upscaling: галочка Active, Method: NVIDIA DLSS (EXTRA FX тоже должен быть включён). С DLAA (Quality: DLAA (100%)) поставьте ещё Сглаживание (MSAA) на «Выкл.» и снимите галочку «Быстрое сглаживание (FXAA)» в Настройки, Assetto Corsa, Графика, а постобработку оставьте включённой, иначе за движущимися объектами будет шлейф. Затем запустите игру как обычно.
5. Выезжайте. Генерация включается сама, как только заработает DLSS в CSP. В паузе и в меню она выключена.

**Обновление:** запустите `install.bat` новой сборки поверх старой. Он сам заменит файлы старой сборки, в том числе программу удаления в `ac-dlssg`, уберёт те, что новой больше не нужны, и сам переключится между режимом с ReShade и без него, если ReShade с тех пор поставили или убрали. Остановится он только на файлах, которые явно не от этого проекта, например на `dxgi.dll` другого мода. Streamline он заново не скачивает, если установленные файлы целы (у каждого закреплённый SHA-256 и подпись NVIDIA; тогда он пишет "Streamline 2.14.1 is already installed and verified; not downloaded again"), и dlssg_for_sm86 на RTX 30 тоже.

**Что меняется в папке игры:** `dxgi.dll` (это мост; с ReShade вместо него `ac-dlssg.dll` и две строки в `ReShade.ini`), папка `ac-dlssg` (Streamline в `sl`, `ac-dlssg.ini`, `logs`, запись об установке в `install`, а также `uninstall.bat`, `collect-logs.bat` и папка `scripts` для них), Lua-приложение CSP в `apps\lua\AcDlssg`, а на RTX 30 ещё `version.dll` и `dlssg_sm86.ini` рядом с `acs.exe`. Больше ничего.

**Для опытных:** `install.bat -NoSpoof` ставит без dlssg_for_sm86; на RTX 30 генерации кадров тогда не будет.

## Как пользоваться

- **Ctrl+F10** включает и выключает генерацию, так удобно сравнить одну и ту же сцену.
- Счётчик FPS в CSP показывает настоящие кадры. Чтобы видеть кадры на экране, нужен внешний счётчик: оверлей NVIDIA (Alt+R), RTSS и т. п.
- Настройки лежат в `<игра>\ac-dlssg\ac-dlssg.ini`: `start_with_fg`, `hotkey`, `log_level`, `fg_multiplier` (2, 3 или 4; его тоже записывает **Save as default**) и `fg_vram_headroom_mib`.
  - `fg_vram_headroom_mib=auto` (по умолчанию) это запас видеопамяти сверх того, что нужно генерации, подобранный под видеокарту: без запаса, если бюджет видеопамяти карты меньше 6 ГБ, и 256 МиБ, если больше. С `auto` генерация включается и тогда, когда для 2X не хватает не больше 128 МиБ (или десятой части нужного, если это больше) и свободна хотя бы половина нужного. Число МиБ вместо `auto` держит свободным ровно столько и без нехватки генерацию не включает. Если генерацию выключает именно это число, а с `auto` она бы работала, мод исправляет это сам: сразу переходит на `auto`, сохраняет `fg_vram_headroom_mib=auto` в `ac-dlssg.ini`, а окно пишет "Video memory setting fixed". Перезапуск не нужен.
  - При обновлении старый вариант по умолчанию `fg_vram_headroom_mib=512` или `=0` (со строкой комментария, которую над ним записал установщик) заменяется на `auto`; значение, которое вы поставили сами, остаётся.
- Окно **AC DLSS-G** (панель приложений CSP, в обоих режимах установки) даёт тот же переключатель, показывает, почему генерация выключена, настоящие и итоговые FPS, время моста на GPU и видеопамять, а кнопка **Save as default** сохраняет текущие переключатели в `ac-dlssg.ini`.
  - **2X / 3X / 4X** выбирают, сколько кадров показывается на один настоящий; выбор действует сразу. 3X и 4X нужны видеокарта и драйвер, которые их позволяют: RTX 50, или RTX 30 и 40 через dlssg_for_sm86, если генерация кадров NVIDIA на них сообщает о поддержке нескольких кадров. Кнопка выше того, что позволяет карта, неактивна ("not supported by this GPU/driver"), а под кнопками написано, когда используется меньший множитель и почему (предел карты или нехватка видеопамяти для большего).
  - Под строкой видеопамяти оранжевым пишется "video memory is tight ... frame generation on anyway", если мост включил генерацию почти без запаса памяти. Если он держит её выключенной, строка состояния пишет "Off: not enough video memory ... lower CSP texture quality, shadows or the render resolution".
  - После **Save as default** окно пишет **Restart the game to apply**: сохранённые переключатели будут при следующем запуске игры (в текущей сессии они уже действуют).

## Удаление

Откройте папку игры (в Steam: правой кнопкой по Assetto Corsa, Управление, Просмотреть локальные файлы), в ней папку `ac-dlssg`, и дважды щёлкните `uninstall.bat`. Он вернёт всё, что поменял установщик, и уберёт мост, Streamline, Lua-приложение и файлы dlssg_for_sm86, которые положил установщик; dlssg_for_sm86, стоявший у вас раньше, остаётся. После Enter в конце он удаляет и себя, `collect-logs.bat` и `scripts`. Логи и настройки остаются в `<игра>\ac-dlssg`. Чтобы удалить и их, запустите там `uninstall.bat -RemoveData`: так удаляется вся папка `<игра>\ac-dlssg`, а также логи и кэш dlssg_for_sm86, если его ставил установщик. То же делает `tools\uninstall.bat` из распакованной папки, если она у вас ещё есть.

## Если что-то не так

- **Игра запускается, но FPS не меняется.** Посмотрите `<игра>\ac-dlssg\logs\bridge.log`. Строка `fg: DLSS-G on` значит, что генерация работает. Строки `fg: frame without DLSS-G: <причина>` или `DLSS-G is not supported on this adapter (...)` объясняют, почему нет.
- **"Not enough video memory" в окне.** Генерации нужно столько памяти, сколько написано в окне. Снизьте в CSP качество текстур, теней или разрешение рендера; мост проверяет снова каждые 60 кадров и включает генерацию, когда памяти хватает.
- **"Bridge and window versions differ" в окне.** Мост и приложение AC DLSS-G из разных релизов. Закройте игру и запустите `install.bat` одного релиза ещё раз.
- **Игра вылетает или не запускается.** Сначала дважды щёлкните `<игра>\ac-dlssg\collect-logs.bat` и сохраните zip (см. следующий пункт), затем запустите `<игра>\ac-dlssg\uninstall.bat`, игра вернётся в прежнее состояние. Zip останется в `<игра>\ac-dlssg`.
- **Сообщить о проблеме:** после закрытия игры дважды щёлкните `<игра>\ac-dlssg\collect-logs.bat`. Он только читает файлы и кладёт один zip в `<игра>\ac-dlssg`; полный путь к нему окно пишет в конце. Приложите этот zip к [issue на GitHub](https://github.com/flopsy2iqq/ac-dlssg/issues).

## Ограничения

- 3X и 4X работают только там, где их позволяют видеокарта и драйвер (RTX 50, или RTX 30 и 40 через dlssg_for_sm86); в остальных случаях мост использует 2X и пишет об этом в окне.
- Окна приложений CSP и интерфейс на экране при быстром движении могут давать мелкие артефакты. Буфер без интерфейса запланирован.
- Не поддерживаются: VR, тройные мониторы, HDR, MSAA, `EXCLUSIVE_FULLSCREEN=1`, вывод с чёрными полосами.
- Другие апскейлеры CSP (FSR, XeSS) не дают данных, которые нужны генерации кадров.

## Про dlssg_for_sm86

NVIDIA разрешает DLSS-G только на RTX 40 и новее. Сторонний [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) от sdli1995 (с работой Coldwood1026 для RTX 20) снимает это ограничение на RTX 30. Автор согласен, что этот проект ссылается на него.

- ac-dlssg его не содержит и не меняет. На RTX 30 установщик скачивает на ваш ПК ровно `version.dll` и `dlssg_sm86.ini` версии 0.3.5 (коммит `9621db5`) из репозитория автора, до установки проверяет их git-хеши (и SHA-256 `version.dll`) и сначала выводит это предупреждение. `version.dll`, который лежал в папке игры до установки, он никогда не заменяет.
- У того репозитория нет файла LICENSE. В README сказано, что исходники под GPLv3, но они не опубликованы.
- Он содержит `nvngx_dlssg.dll` от NVIDIA, лицензия на который не менялась.
- Его использование на RTX 30 обходит техническое ограничение, что запрещает пункт 4.d лицензии NVIDIA RTX SDKs для её лицензиатов.

Ставить его или нет, решаете вы: `install.bat -NoSpoof` ставит без него, а `<игра>\ac-dlssg\uninstall.bat` его убирает.

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

- [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) от sdli1995, с работой Coldwood1026: DLSS-G на RTX 30. Установщик скачивает его из репозитория автора на вашем ПК.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline): SDK, через который работают DLSS Frame Generation и Reflex. Его DLL скачиваются с релиза NVIDIA на вашем ПК.
- [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (MIT): способ перехватить вызов DLSS в CSP.
- [open-shaders](https://github.com/alandtse/open-shaders) и [Community Shaders](https://github.com/doodlum/skyrim-community-shaders): схема «игра на DirectX 11 плюс цепочка обмена DirectX 12 через Streamline».

## Проверка скачанного архива

Каждый архив релиза собирает GitHub Actions из этого репозитория, а у архива и `ac-dlssg.dll` есть аттестация сборки. В описании релиза указан SHA-256 обоих файлов. Проверить скачанный файл через GitHub CLI:

```
gh attestation verify ac-dlssg-1.0.0.zip --repo flopsy2iqq/ac-dlssg
```

## Поддержать автора

ac-dlssg бесплатный, и все функции останутся бесплатными. Если хотите сказать спасибо, можно [угостить автора кофе на Ko-fi](https://ko-fi.com/flopsy2iq). Это по желанию.

## Лицензия

GPL-3.0. См. [LICENSE](LICENSE).

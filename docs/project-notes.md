# Ключевые факты о проекте

Исходники игры «Самогонки» / Moonshine Runners (KD-lab, GPLv3). Всё, что нужно
чтобы не переоткрывать одно и то же заново.

## Окружение сборки

Инструментарий живёт в `C:\msys64\ucrt64` (не `mingw64` — давние кэши
прокрутят путь на несуществующий `C:/msys64/mingw64/bin/c++.exe`).

```
cmake -B build-ucrt -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe \
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe \
      -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build-ucrt -j 8
```

* Сокеты Windows: `ws2_32` уже в линковке (`MechoSoma/CMakeLists.txt`).
* `3rdparty/SDL-2.26.5/include` и `lib/libSDL2.a` должны указывать на
  `C:\msys64\ucrt64` — иначе `-lSDL2` подхватывает `libSDL2.dll.a` и exe
  начинает требовать `SDL2.dll` рядом.
* `bin/` — готовая к запуску папка с ресурсами и `lang/<код>/RESOURCE/...`.
  Запуск: `bin/moonshine-runners.exe -lang ru`.

## Главное меню

Меню целиком описано данными: `iscreen.scb` собирается из
`iScreen/SCRIPTS/ISCREEN.SCR` плюс `ru_/en_/lt_` варианты. Пункт меню — это
`ACS_INPUT_FIELD_OBJ` с `event`, который шлёт `ACS_EXT_EVENT` в
`acsHandleExtEvent()` (`iScreen/ACS_EXT.CPP`).

* Геометрия: `x`, `y` — строки с выражениями, вычисляются парсером по
  `MAIN_MENU_SX/SY/DELTA/SIZE`. Индекс `*_Y` задаёт позицию в списке.
* Навигация с клавиатуры — `objIndex[4]` (0 влево, 1 вверх, 2 вправо, 3 вниз).
  Не указан — геометрический поиск ближайшего подходящего
  (`ACScore::GetObjectXY`).
* Строки: `aci_str.inc` + `aci_ids.h`. Кодировка **cp1251**, для русского.
* Пересборка `iscreen.scb` — только утилитой `script-tool` с оригинальным
  датасетом `lang/`. Без неё правки SCR не доезжают до игры, поэтому новое
  меню имеет C++-фолбэк: `acsAddWebSiteOption()` (`ACS_EXT.CPP`).

## Симуляция

* Фиксированный шаг: `XrealEvolveQuant()` раз в 13 мс виртуального времени
  (`mechosoma.cpp:2973`), `evolve_time_step = 0.07f`
  (`gen_xreal.cpp:18`). Переменный `dt` есть только в планировщике кадров.
* `SystemResourceDispatcher::evolve(N * 13)` крутит симуляцию
  детерминированно — этим уже пользуется реплей PBEM (`mechosoma.cpp:3288`).
* «Реальное время» = `mchGameMode == MCH_SINGLE_GAME && !mchTurnBasedGame &&
  !iWorldMode` (`mechosoma.cpp:801-834`).
* Гейт всей симуляции — флаг `mchFreezeTime`.
* Прямое управление с клавиатуры уже работает: `SetPointControl()`
  (→ `controled_only_by_point = 1`) вызывается только для hotseat
  (`race.cpp:1037-1038`), поэтому в `MCH_SINGLE_GAME` при пустом
  `control_points()` управление идёт в `keyboard_control()`
  (`Xreal/Mechos.cpp:1367`).

## Сеть

Библиотек сети нет, кроме собственного транспорта:

* `RUNTIME/net_protocol.h` + `net_protocol_types.h` — форматы сообщений.
* `RUNTIME/net_socket.cpp` — единственное место с `<winsock2.h>`.
* `RUNTIME/net_transport.cpp` — сессии, пиры, надёжность, фрагментация.
* `RUNTIME/net_selftest.cpp` — самотест флагом `-nettest`.

**Почему сокеты изолированы.** `windows.h` конфликтует с двумя вещами, которые
проект объявляет сам: `enum dbtype3ds { Unknown }` в `3dsftk.h` и
`WritePrivateProfileString` в `XTool/port.h`. Поэтому TU с winsock не
включает проектных заголовков, а `net_selftest.cpp` вместо `windows.h`
объявляет `extern "C" void Sleep(unsigned int)`.

## Что не работает и почему

| Что | Состояние |
|---|---|
| `RUNTIME/wininet_api.cpp` | Заглушка `wininet_api_port.cpp`. `#ifdef NETWORK` никогда не определён |
| `RUNTIME/online_game.cpp` | Мёртвый код: legacy HTTP PBEM, `ogQuant()` вызывается только под `#ifdef NETWORK` |
| `hsRndSeed` / `ogPacket::GAME_RND` | Записываются в `XRndVal`, который является логовой переменной и **не** сидит генераторе. Общего «сида матча» в движке нет |
| Пункт «Сайт» | Есть, `-lang ru` → «САЙТ», открывает `https://kdlab.com` |

## Как открывать URL

`SDL_OpenURL()` — кроссплатформенно, включая Emscripten. Обёрнута в
`acsOpenWebSite()` (`iScreen/ACS_EXT.CPP`).

## Локализация

`iScreen/SCRIPTS/ACI_STR.INC` (генерируемый, cp1251). Подпись пункта берётся
из текущего языка: `localization::getLanguage()`
(`XTool/filesystem.h`).

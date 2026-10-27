# Eternity Warriors 2 Bridge (PC)

Мост для запуска **Eternity Warriors 2 v4.3.1** (`com.glu.ewarriors2`, Android `armeabi-v7a`, Unity 4.2.1f4 + Mono) на ПК
без эмулятора Android: оригинальные ARM-библиотеки игры исполняются под `qemu-arm` (user mode), а Android-окружение
(Bionic ELF loader, JNI, FMOD, сеть, сохранения) заменяет собственный загрузчик. Графика идёт по сети в нативное окно:
OpenGL ES 2.0 на видеокарте Windows (ANGLE → Direct3D 11) или на Mesa в WSLg. Архитектура по образцу
[shadowgun-bridge](https://github.com/JamesCrumble/shadowgun-bridge) и Eternity Warriors 3 Bridge.

Файлы игры в репозиторий не входят: нужны ваши собственные APK и OBB.

---

## Содержание

1. [Быстрый старт](#быстрый-старт)
2. [Подробная установка](#подробная-установка)
3. [Запуск и настройки](#запуск-и-настройки)
4. [Управление](#управление)
5. [Сохранения](#сохранения)
6. [Производительность: что сделано](#производительность-что-сделано)
7. [Логи и профилирование](#логи-и-профилирование)
8. [Если что-то не работает](#если-что-то-не-работает)
9. [Архитектура и устройство репозитория](#архитектура-и-устройство-репозитория)
10. [Сборка отдельных частей](#сборка-отдельных-частей)
11. [Справочник переменных окружения](#справочник-переменных-окружения)
12. [Лицензии](#лицензии)

---

## Быстрый старт

Нужны Windows 10/11 с **WSL2** (Ubuntu 20.04 или новее, с WSLg), ваши `com.glu.ewarriors2-435.apk` и
`main.431.com.glu.ewarriors2.obb`.

```bash
# в WSL, в каталоге проекта
sudo dpkg --add-architecture armhf && sudo apt update
sudo apt install -y python3 curl qemu-user gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf gcc-mingw-w64-x86-64 \
    libsdl2-dev:armhf libzip-dev:armhf libbsd-dev:armhf zlib1g-dev:armhf \
    libsdl2-2.0-0 libgles2 libegl1 libegl1-mesa-dev libgles2-mesa-dev

mkdir -p gamebin && cp /путь/к/com.glu.ewarriors2-435.apk /путь/к/main.431.com.glu.ewarriors2.obb gamebin/
tools/setup_deps.sh          # SDL2 и ANGLE для запуска на видеокарте (один раз)
qemu/build.sh                # свой qemu-arm, быстрее системного (один раз, нужен Docker; можно пропустить)
./play_wsl.sh                # окно 1280x720; ./play_wsl.sh 1920x1080 - другое разрешение
```

Из Windows игру можно запускать двойным кликом по **`play_win.cmd`** (он вызывает тот же `play_wsl.sh` через WSL).
Первый запуск дольше: распаковка игры, сборка порта и подготовка бандлов (минуты); дальше старт занимает секунды.

---

## Подробная установка

### Что нужно

| Что | Зачем |
|---|---|
| Windows 10/11 x64 + WSL2 (Ubuntu 20.04+) с WSLg | игра запускается из WSL; окно, звук и ввод идут через WSLg/Windows |
| Видеокарта с Direct3D 11 | рендер OpenGL ES через ANGLE; без неё есть программный режим `EW2_HOST=linux` (медленнее) |
| APK `com.glu.ewarriors2-435.apk` и OBB `main.431.com.glu.ewarriors2.obb` | игра; кладутся в `gamebin/` |
| Docker (по желанию) | сборка собственного `qemu-arm` в контейнере; ничего не ставится на хост |
| ~3 ГБ свободного места | распакованная игра, кэш бандлов (~300 МБ), сборки |

Всё остальное (зависимости сборки, кэши, собранные бинарники) лежит внутри проекта; глобально ставятся только пакеты
`apt` из списка ниже.

### 1. WSL2

В администраторском терминале Windows: `wsl --install -d Ubuntu-20.04`, затем перезагрузка. Проект лучше держать в
файловой системе WSL (`~/...`), а не на диске `C:` (иначе сборки и кэши будут медленными). Проверка WSLg:
`ls /mnt/wslg` должен показать каталог с `PulseServer`.

### 2. Пакеты

```bash
sudo dpkg --add-architecture armhf && sudo apt update
sudo apt install -y python3 curl qemu-user \
    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf gcc-mingw-w64-x86-64 \
    libsdl2-dev:armhf libzip-dev:armhf libbsd-dev:armhf zlib1g-dev:armhf \
    libsdl2-2.0-0 libgles2 libegl1 libegl1-mesa-dev libgles2-mesa-dev
```

- `gcc/g++-arm-linux-gnueabihf` и armhf-библиотеки собирают ARM-порт (`port/build/ew2`);
- `gcc-mingw-w64-x86-64` собирает `ew2gl.exe`, GL-мост для видеокарты Windows;
- `qemu-user` нужен как запасной эмулятор, если не собран свой;
- `libsdl2-2.0-0 libgles2 libegl1` нужны нативному хосту (`EW2_HOST=linux`) и приёмнику звука.

### 3. Файлы игры

Положите APK и OBB в `gamebin/`. При первом запуске `tools/setup_ew2data.py` распакует их в `ew2data/` (библиотеки,
расшифрованный `glu_properties.txt`, структура OBB). Файлы в `gamebin/` не изменяются: OBB копируется и оптимизируется
только копия в `ew2data/`.

### 4. Зависимости GPU-хоста

```bash
tools/setup_deps.sh
```

Скрипт кладёт в проект: заголовки Khronos (из системных `/usr/include`), SDL2 для Windows (скачивает
`SDL2-devel-2.30.9-VC.zip` с https://github.com/libsdl-org/SDL/releases, либо `--sdl2 путь.zip`) и ANGLE
(`libEGL.dll`, `libGLESv2.dll`). У ANGLE нет официальных бинарников, поэтому DLL берутся из программы, которая их
поставляет: скрипт ищет в OBS Studio, Google Chrome и Microsoft Edge, либо `--angle "папка с DLL"`. Без этих файлов
запуск возможен только с `EW2_HOST=linux`.

### 5. Свой qemu-arm (рекомендуется)

```bash
qemu/build.sh
```

Собирает в контейнере QEMU 11.1.2 с патчами из `qemu/patches/` и кладёт результат в `third_party/qemu/qemu-arm`
(~1 минута). `play_wsl.sh` использует его автоматически. Исходники берутся из архива `qemu-11.1.2.tar.xz`
(по умолчанию из соседнего проекта `../nova3probe/downloads/`, либо `QEMU_TARBALL=путь`, оригинал:
https://download.qemu.org/qemu-11.1.2.tar.xz). Без него используется системный `qemu-arm`: игра работает, но ~20%
медленнее.

### 6. Запуск

```bash
./play_wsl.sh
```

---

## Запуск и настройки

| Способ | Что делает |
|---|---|
| `./play_wsl.sh [WxH]` | основной запуск из WSL |
| `play_win.cmd [WxH]` | двойной клик или терминал Windows; запускает `play_wsl.sh` через WSL |
| `python3 play_win.py [WxH]` | то же из WSL/Linux (передаёт управление `play_wsl.sh`) или из Windows (через `wsl.exe`) |
| `EW2_HOST=linux ./play_wsl.sh` | окно WSLg на Mesa (программный рендер), не нужны ANGLE и SDL2 для Windows |
| `tools/play_win.sh`, `tools/play_gpu.sh`, `tools/play_wslg.sh` | тонкие обёртки над `play_wsl.sh` (последняя включает `EW2_HOST=linux`) |

Что делает `play_wsl.sh`:

1. при необходимости распаковывает игру из `gamebin/` в `ew2data/`;
2. один раз распаковывает LZMA-бандлы OBB и кладёт их в `UnityCache` (иначе загрузка уровней висит секундами);
3. собирает ARM-порт и SDL-шим (`tools/build_port_wsl.sh`), GL-мост для Windows (`tools/build_host_win.sh`) и
   приёмник звука (`tools/audio_sink.c`), если исходники изменились;
4. архивирует логи прошлого запуска в `logs/history/<время>/` (хранятся последние 20);
5. поднимает HTTP-заглушку серверов Glu (`tools/http_stub.py`, порт 18080) и приёмник звука;
6. запускает игру под `qemu-arm` и окно с рендером; закрытие окна или Ctrl+C останавливает всё.

Полезные переменные (полный список в [справочнике](#справочник-переменных-окружения)):

```bash
EW2_TARGET_FPS=60 ./play_wsl.sh                       # ограничить частоту кадров (по умолчанию 120)
EW2_GL_VSYNC=1 ./play_wsl.sh                          # вертикальная синхронизация
EW2_QEMU=/путь/к/qemu-arm ./play_wsl.sh               # другой эмулятор
EW2_AUDIO_LATENCY_MS=20 ./play_wsl.sh                 # меньше задержка звука
EW2_JOB_THREADS=0 ./play_wsl.sh                       # отключить рабочие потоки Unity
EW2_FIND_CACHE=0 ./play_wsl.sh                        # отключить кэш FindObjectsOfType
```

---

## Управление

Раскладка задаётся в `ctl/keymap.txt` и `host/glbridge/ew3gl_input.inc`.

| Клавиша / мышь | Действие в игре | Описание |
|---|---|---|
| **W, A, S, D** (или стрелки) | Перемещение | Бег в 8 направлениях (изометрия, виртуальный джойстик) |
| **ЛКМ** | Основная атака | Серия ударов топором в боевом режиме (`F1` переключает режим мыши) |
| **ПКМ** | Блок / щит | Защита щитом и парирование (`Skill 2`) |
| **Пробел** | Рывок | Быстрый кувырок / уклонение (`Skill 3`) |
| **СКМ (зажать)** | Заряженная атака (вихрь) | Вращение с топором (`Skill 1`), направление клавишами `WASD` |
| **1** | Зелье здоровья | Мгновенное восстановление здоровья |
| **2** | Зелье ярости | Мгновенное восполнение шкалы ярости |
| **F1** | Режим мыши | `touch` — клики по меню/инвентарю; `battle` — ЛКМ атакует |
| **Esc** | Пауза / назад | Android `KEYCODE_BACK` |
| **F2** | Счётчик FPS | Вкл/выкл оверлей |
| **F11** | Полный экран | Оконный / полноэкранный режим |
| **F12** | Скриншот | Кадр сохраняется в `ctl/` |

---

## Сохранения

Игра хранит сохранения в **`Saves/`** (`ew2data/files` — ссылка на этот каталог):

- `Saves/savegame.bin` — основной файл (персонаж, уровень, золото, кристаллы, инвентарь, прогресс);
- `Saves/playerprefs.txt` — настройки `PlayerPrefs`;
- `Saves/UnityCache/` — кэш бандлов (создаётся автоматически, ~300 МБ, можно удалить; не коммитится);
- `Optional_Cheat_Save/savegame.bin` — необязательное сохранение с бесконечной валютой (999 999 999 монет и кристаллов).
  Установка: скопируйте его поверх `Saves/savegame.bin` (см. `Optional_Cheat_Save/HOW_TO_INSTALL_CHEAT_SAVE.txt`).

Файл `Saves/savegame.bin` отслеживается git и меняется при каждой игре; не включайте его в коммит.

---

## Производительность: что сделано

Игра исполняется эмулятором, поэтому каждый кадр дорог. Итоговые меры (в порядке влияния):

- **Собственный `qemu-arm`** (`qemu/`): QEMU 11.1.2, без защитных флагов компилятора, с патчами `nova3.patch`
  (быстрый поиск трансляций, большой кэш переходов) и `x86_neon.patch` (одноточечный NEON на SSE: код движка для
  скиннинга считается примерно в 4 раза быстрее; результат побитово совпадает с softfloat, при NaN и денормалах
  расчёт уходит на штатный путь; `QEMU_STRICT_FP=1` отключает). На aarch64-хосте (Android) `nova3.patch` включает
  собственные быстрые FP/NEON; дополнительные патчи для него лежат в `qemu/patches/android/`.
- **Кэш `FindObjectsOfType`** (`port/src/find_cache.cpp`): игра запрашивает `HomeUIController` каждый кадр, а Unity 4
  обходит для этого всю таблицу объектов (~4 мс, около 30% кадра в бою). Результат хранится и сбрасывается при
  создании/уничтожении объектов, загрузке уровня и через 30 кадров. `EW2_FIND_CACHE=Тип1,Тип2` меняет список типов.
- **Рабочие потоки Unity** (`JobScheduler`): скиннинг, культинг и анимация идут параллельно (около −10% кадра в
  плотном бою).
- **Предварительная распаковка бандлов** (`tools/optimize_obb.py`).
- **Кэш шейдеров на хосте** (`ew2data/shader_cache`, `EGL_ANDROID_blob_cache`): ANGLE компилирует шейдер на первом
  кадре, где он нужен (30–160 мс); со второго запуска пропадает.
- **Заглушка PvP-серверов** отвечает `503`, как недоступный сервер: игра больше не пытается разобрать фиктивный ответ
  (исключение и зависание на 1–3 секунды).
- **Асинхронный `trace()`**: вывод в лог не блокирует поток движка.

Особенности `qemu-arm` в user mode, которые порт обходит:

- `EW2_NO_CNTVCT=1` (выставляется скриптом): счётчик CNTVCT доступен только в `qemu-system-arm`; время берётся через
  `clock_gettime`.
- `/proc/cpuinfo` в qemu-user — это x86 хоста. Mono (`thumb_supported`) и FMOD (`failed to get number of drivers`) по
  нему выбирают код, поэтому порт подменяет файл сгенерированным ARM-вариантом; порт собран в режиме ARM (`-marm`).
- FMOD инициализируется внутри `unityAndroidInit`, поэтому `fmodInitJni` вызывается раньше.

Что остаётся дорогим: пачка новых монстров добавляет ~10 мс на кадр спавна (скиннинг, анимация, обход сцены — это
нативный код движка, а не C#), а начало и конец уровня дают 1–3 секунды из-за обхода полей скриптов в самом Mono.

---

## Логи и профилирование

Все логи в `logs/`; каждый запуск перемещает предыдущие в `logs/history/<время>/`.

| Файл | Содержимое |
|---|---|
| `logs/play.log` | вывод гостя (игра, порт); `TRACE:`-строки только с `LOADER_TRACE=1` |
| `logs/ew2gl.log` | хост рендера: FPS, команды GL, время по операциям |
| `logs/audio.log`, `logs/http.log` | приёмник звука, заглушка серверов |

Максимально подробный лог (профайлеры, лог игры, трассировка JNI, зависания от 15 мс):

```bash
LOADER_TRACE=1 EW2_PROFILE=1 EW2_UNITY_LOG=1 EW2_SLOW_FRAME_MS=16 EW2_GL_STATS_EVERY=120 EW2_GL_REPORT=1 \
EW3_CRASH_MAPS=1 EW3_JNI_TRACE=1 EW3_INIT_TRACE=1 ./play_wsl.sh
```

`EW2_PROFILE=1` включает: `glprof` (время кадра и разбивка Unity), `slow frame N` (причина каждого зависания),
`pcprof`/`pcslow` (выборки по адресам), `sysprof` (системные вызовы), `icallprof` (стоимость `Find*`, `Instantiate`,
`Resources.Load`). Профайл только медленных кадров, например кадров спавна (15–60 мс):

```bash
EW2_PROFILE=1 EW2_PC_PROF_HZ=1000 EW2_PC_PROF_SLOW_MS=15 EW2_PC_PROF_SLOW_MAX_MS=60 EW2_PC_PROF_SLOW_REPORT=500 ./play_wsl.sh
```

Режимы, которые заметно замедляют игру и искажают тайминги (для поиска ошибок, не для замеров):
`EW3_GL_DIAG=1` (`glGetError` после вызовов; `EW2_GL_DIAG_EVERY=N` проверяет только каждый N-й кадр),
`EW2_GL_CHECK=1` (то же на хосте; `EW2_GL_CHECK_EVERY=N`), `QEMU_STRACE=1` (все системные вызовы гостя).

Разбор логов:

```bash
python3 tools/log_digest.py [play.log ew2gl.log]   # сводка: FPS, зависания, горячие функции
python3 tools/timeline.py logs/play.log            # хронология кадров и событий
python3 tools/guest_prof.py logs/play.log          # горячие функции гостя
python3 tools/sync_ops.py logs/ew2gl.log           # синхронные GL-запросы хоста
```

При сравнении замеров не запускайте во время игры ничего тяжёлого (сборки, вторую копию игры): иначе цифры
недостоверны (сборка qemu на всех ядрах роняет FPS втрое).

---

## Если что-то не работает

| Симптом | Причина и решение |
|---|---|
| `FileNotFoundError: taskkill` или `zig.exe` в старых версиях `play_win.py` | запуск Windows-лаунчера из Linux; используйте `./play_wsl.sh` или актуальный `play_win.py` |
| `no QEMU tarball` при `qemu/build.sh` | положите `qemu-11.1.2.tar.xz` рядом или задайте `QEMU_TARBALL=путь` |
| окно не открывается, в `logs/ew2gl.log` ошибка ANGLE/SDL | выполните `tools/setup_deps.sh`; без GPU-зависимостей работает `EW2_HOST=linux ./play_wsl.sh` |
| `GL_RENDERER` в `logs/ew2gl.log` — `llvmpipe` | запущен программный рендер (`EW2_HOST=linux` или GPU-хост не собрался); проверьте `tools/build_host_win.sh` |
| нет звука | проверьте `ls /mnt/wslg/PulseServer` и `logs/audio.log` (`[audio] opened 24000 Hz`); нужен WSLg |
| `Illegal instruction` в `play.log` сразу после старта | запуск `qemu-arm` без обхода CNTVCT: запускайте через `play_wsl.sh` (он ставит `EW2_NO_CNTVCT=1`) |
| `Assertion ... thumb_supported` | порт собран не в режиме ARM или не подменён `cpuinfo`: пересоберите `tools/build_port_wsl.sh` |
| зависания на 1–3 с на каждом старте | первый запуск после обновления шейдеров/бандлов; повторите, кэши заполнены |
| после остановки остались процессы | `pkill -f qemu-arm; pkill -f ew2gl` (скрипт убирает их сам при закрытии окна и Ctrl+C) |

---

## Архитектура и устройство репозитория

```
play_win.cmd / play_win.py / play_wsl.sh
  │
  ├─► Окно рендера и ввода
  │     ├── host/build-win/ew2gl.exe  (Windows-процесс из WSL: ANGLE → Direct3D 11, видеокарта)
  │     └── build/ew2gl               (EW2_HOST=linux: Mesa в WSLg)
  │          ├── разбор потока команд OpenGL ES по TCP (:18090)
  │          ├── ввод W/A/S/D/мышь → мультитач Unity (ctl/commands)
  │          └── кэш шейдеров (ew2data/shader_cache)
  │
  ├─► Гость: qemu-arm port/build/ew2 ew2data
  │     ├── Bionic ELF loader: libunity.so (Unity 4.2.1f4), libmono.so, libmain.so из APK/OBB
  │     ├── динамическая JNI-машина: UnityPlayer, PlayerPrefs, FMODAudioDevice, плагины Glu, UnityWWW
  │     ├── чтение ассетов из base.apk и main.431.com.glu.ewarriors2.obb
  │     └── сохранения: ew2data/files → Saves/
  │
  ├─► Звук: FMOD → fifo → build/audio_sink (SDL2 → PulseAudio WSLg)
  └─► tools/http_stub.py (:18080): заглушка серверов Glu (версии, PvP → 503)
```

| Каталог | Что внутри |
|---|---|
| `port/` | загрузчик и среда гостя (C++): `src/`, `loader/`, `jni/`, `thunks/` |
| `host/glbridge/` | хост рендера `ew3gl.c` и модули `*.inc` (общий код для Windows и Linux) |
| `qemu/` | Dockerfile, `build.sh` и патчи для собственного `qemu-arm` |
| `tools/` | сборки (`build_*.sh`), подготовка данных (`setup_*`), анализ логов |
| `third_party/` | внешние зависимости (в git не входят: `qemu/`, `angle/`, `sdl2-win/`, `khronos/`; `qemu-guest/` — старый VM-путь) |
| `gamebin/` | ваши APK и OBB (в git не входят) |
| `ew2data/` | распакованная игра, кэши (в git не входят) |
| `Saves/`, `Optional_Cheat_Save/` | сохранения |
| `logs/`, `logs_saved/` | логи (в git не входят) |

Старый путь «всё на Windows» (виртуальная машина `qemu-system-arm`, `EW2_LEGACY_VM=1 python play_win.py`) сохранён в
`play_win.py` и `third_party/qemu-guest/`, но требует файлов, которых в репозитории нет (xPack QEMU, `vmlinuz`,
`ew2_initrd.cpio`), и не поддерживается.

---

## Сборка отдельных частей

| Что | Команда | Результат |
|---|---|---|
| ARM-порт и SDL-шим | `tools/build_port_wsl.sh` | `port/build/ew2`, `sdl-arm/lib/libSDL2-2.0.so.0` |
| GL-мост для GPU Windows | `tools/build_host_win.sh` | `host/build-win/ew2gl.exe` |
| GL-мост Linux (Mesa) | собирается из `play_wsl.sh` | `build/ew2gl` |
| qemu-arm | `qemu/build.sh` (Docker) | `third_party/qemu/qemu-arm` |
| Данные игры | `tools/setup_ew2data.py` | `ew2data/` |
| Зависимости GPU-хоста | `tools/setup_deps.sh` | `third_party/{khronos,sdl2-win,angle}` |

Варианты qemu: `QEMU_VARIANT=имя QEMU_PATCHES="..." qemu/build.sh` собирает бинарник
`third_party/qemu/qemu-arm-имя` (например, без `x86_neon.patch` для сравнения скорости). Для Android-сборки патчи
`qemu/patches/nova3.patch` и `qemu/patches/android/*.patch` рассчитаны на aarch64-хост (под `#if defined(__aarch64__)`).

---

## Справочник переменных окружения

Переменные `EW2_*` читают и порт (гость), и хост рендера; `EW3_*` — унаследованные отладочные переключатели.

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `EW2_HOST` | `gpu` | `gpu` — `ew2gl.exe` на видеокарте Windows; `linux` — Mesa в WSLg |
| `EW2_QEMU` | `third_party/qemu/qemu-arm`, иначе системный | какой `qemu-arm` использовать |
| `EW2_TARGET_FPS` | 120 | ограничение частоты кадров |
| `EW2_GL_VSYNC` | 0 | вертикальная синхронизация |
| `EW2_GL_RENDERER` | `Adreno (TM) 420` | имя GPU, которое видит игра (влияет на выбор качества) |
| `EW2_JOB_THREADS` | включено | `0` — однопоточный `JobScheduler` |
| `EW2_FIND_CACHE` | `HomeUIController` | типы для кэша `FindObjectsOfType`; `0` — выключить |
| `EW2_FIND_CACHE_FRAMES` | 30 | максимальный возраст записи кэша, кадров |
| `EW2_AUDIO_LATENCY_MS` | 40 | желаемая задержка PulseAudio |
| `EW2_SHADER_CACHE` | `ew2data/shader_cache` | каталог кэша шейдеров хоста |
| `EW2_GL_PORT` | 18090 | порт GL-моста |
| `EW2_WSL_DISTRO` | — | дистрибутив WSL для `play_win.cmd` и `play_win.py` |
| `EW2_LEGACY_VM` | — | `1` — старый VM-путь в `play_win.py` |
| `EW2_NO_CNTVCT` | выставляет скрипт | не читать CNTVCT (нужно под qemu-user) |
| `LOADER_TRACE` | 0 | `1` — `TRACE:`-строки в `logs/play.log` |
| `EW2_TRACE_SYNC` | 0 | `1` — синхронный `trace()` (без буфера) |
| `EW2_PROFILE` | 0 | `1` — профайлеры (`glprof`, `pcprof`, `sysprof`, `icallprof`) |
| `EW2_UNITY_LOG` | 0 | `1` — пропускать `Debug.Log` игры в лог |
| `EW2_SLOW_FRAME_MS` | 25 | порог «зависания» для `slow frame` |
| `EW2_PC_PROF_HZ` | 250 | частота выборок профайлера |
| `EW2_PC_PROF_SLOW_MS`, `_SLOW_MAX_MS`, `_SLOW_REPORT` | 16, 0, 300 | профиль только медленных кадров (нижний и верхний порог, размер отчёта) |
| `EW2_GL_REPORT` | 2 | период отчёта хоста, секунд |
| `EW2_GL_STATS_EVERY` | — | период статистики GL гостя, кадров |
| `EW2_GL_CHECK`, `EW2_GL_CHECK_EVERY` | 0, 1 | `glGetError` на хосте (медленно) / каждый N-й кадр |
| `EW3_GL_DIAG`, `EW2_GL_DIAG_EVERY` | 0, 1 | то же в госте для загрузки сжатых текстур |
| `EW3_JNI_TRACE`, `EW3_INIT_TRACE`, `EW3_CRASH_MAPS` | 0 | трассировка JNI и инициализации; карта памяти в отчёте о падении |
| `QEMU_STRICT_FP` | — | `1` — отключить быстрые FP/NEON в нашем `qemu-arm` |
| `QEMU_STRACE` | — | `1` — журнал системных вызовов гостя (очень медленно) |

---

## Лицензии

Код проекта: см. `port/LICENSE` (GPL-3.0). Патчи в `qemu/patches/` — производные работы от QEMU (GPL-2.0).
Eternity Warriors 2 и все его материалы принадлежат Glu; проект с Glu не связан и файлы игры не распространяет.

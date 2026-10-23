# Eternity Warriors 2 Bridge (Windows PC Port)

Нативный мост-прослойка для запуска **Eternity Warriors 2 v4.3.1** (`com.glu.ewarriors2`, Android `armeabi-v7a`, Unity 4.2.1f4 + Mono) на Windows 10/11 x64 по архитектуре [shadowgun-bridge](https://github.com/JamesCrumble/shadowgun-bridge) и `Eternity Warriors 3 Bridge` (`qemu-user` / `qemu-system-arm` + Bionic ELF loader + динамический JNI + GLES2-over-TCP -> ANGLE Direct3D 11 + SDL2).

---

## Быстрый старт

Запустите игру двойным кликом по **`play_win.cmd`** или из терминала:

```bat
play_win.cmd
```
По умолчанию окно открывается в `1280x720`. Можно передать любое разрешение аргументом:
```bat
play_win.cmd 1920x1080
```

---

## Управление на ПК (`ctl/keymap.txt` / `host/glbridge/ew3gl_input.inc`)

| Клавиша / Мышь | Действие в игре | Описание |
|---|---|---|
| **W, A, S, D** (или стрелки) | Перемещение | Бег в 8 направлениях (изометрия, виртуальный джойстик) |
| **ЛКМ (LMB)** | Основная атака | Серия ударов топором в боевом режиме (`F1` переключает режим мыши) |
| **ПКМ (RMB)** | Блок / Щит | Защита щитом и парирование (`Skill 2`) |
| **Пробел (Space)** | Рывок (Dash / Roll) | Быстрый кувырок / уклонение (`Skill 3`) |
| **СКМ (MMB / Колёсико, зажать)** | Заряженная атака (Вихрь) | Удерживайте для вращения с топором (`Skill 1`), направляя движение клавишами `WASD` |
| **Клавиша 1** | Зелье здоровья (HP) | Мгновенное восстановление здоровья |
| **Клавиша 2** | Зелье ярости (Rage) | Мгновенное восполнение шкалы ярости |
| **F1** | Режим мыши (`touch` / `battle`) | `touch` — клики курсором по меню/инвентарю; `battle` — ЛКМ атакует |
| **Esc** | Пауза / Назад | Android `KEYCODE_BACK` |
| **F2** | Счётчик FPS | Вкл/выкл оверлей FPS |
| **F11** | Полный экран | Переключение оконного / полноэкранного режима |
| **F12** | Скриншот | Сохранение скриншота кадра |

---

## Локальные сохранения (`Saves/`)

Игра использует локальные сохранения в папке **`Saves/`** (аналогично `Eternity_Warriors_2_PC\Saves`):
- **`Saves/savegame.bin`** — основной файл сохранения (персонаж, уровень, золото, кристаллы, инвентарь, прогресс подземелий).
- **`Saves/playerprefs.txt`** — локальные настройки `UnityEngine.PlayerPrefs`.
- **`Optional_Cheat_Save/savegame.bin`** — опциональное чит-сохранение с бесконечной валютой (`999,999,999` монет и кристаллов / Gems). Чтобы установить его, скопируйте `Optional_Cheat_Save\savegame.bin` с заменой в `Saves\savegame.bin` (см. `Optional_Cheat_Save\HOW_TO_INSTALL_CHEAT_SAVE.txt`).

---

## Архитектура проекта

```
play_win.cmd / play_win.py
  │
  ├─► Окно рендеринга и ввода (host/build-win/ew2gl.exe)
  │     ├── Декодирование потока команд OpenGL ES 2.0/3.0 по TCP (:18090)
  │     ├── Аппаратный рендеринг через Google ANGLE -> Direct3D 11
  │     └── Трансляция WASD / LMB / RMB / MMB / Space / 1 / 2 -> мультитач Unity
  │
  └─► Гостевой ARM32 процесс (port/build/ew2)
        ├── Bionic ELF loader: загрузка оригинальных libunity.so (Unity 4.2.1f4), libmono.so, libmain.so
        ├── Динамическая JNI VM: эмуляция UnityPlayer, PlayerPrefs, FMODAudioDevice, AJavaTools (Glu SDK), UnityWWW
        ├── Чтение ассетов и бандлов напрямую из base.apk и main.431.com.glu.ewarriors2.obb
        └── Синхронизация локальных сохранений (Saves/savegame.bin <-> /ew2data/files/savegame.bin)
```

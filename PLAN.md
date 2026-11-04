# rout — план реализации

Нативное Windows-приложение в духе Nyxt hint-mode / Homerow: по хоткею
показывает поверх всего экрана метки на кликабельных объектах активного
приложения и позволяет "кликнуть" клавиатурой, без мыши.

Целевые приложения на старте: браузер (Chrome/Edge), VS Code, Rider,
Claude Desktop. Остальные приложения — через общий fallback.

## Архитектура

- **Core Engine (C++)** — единственный владелец оверлея и глобального
  хоткея. Рисует метки, перехватывает ввод при открытом оверлее,
  выполняет клик сам только в generic-пути (через UI Automation).
- **Адаптеры** — лёгкие клиенты, подключаются к Core по локальному
  WebSocket (127.0.0.1, фиксированный порт, авторизация токеном из
  файла с ограниченным ACL). Если адаптер способен выполнить действие
  сам (DOM `element.click()`, `commands.executeCommand`, вызов
  IntelliJ-экшена) — делает это сам, а не просит Core кликать по
  координатам. Это снимает проблемы DPI/мультимонитор/фокуса для
  "умных" адаптеров.
- **Protocol v1** (JSON по WS):
  - Core → адаптер: `{"cmd":"get_hints"}`, `{"cmd":"activate","id":"..."}`
  - Адаптер → Core: `{"type":"hello","app":"...","pid":...}`,
    `{"type":"hints","items":[{"id":"...","rect":[x,y,w,h]}]}`,
    `{"type":"ack"}`
- Core хранит таблицу `имя процесса → адаптер`. По хоткею определяется
  foreground-процесс (`GetForegroundWindow` → `GetWindowThreadProcessId`
  → имя exe); если для него есть живой адаптер — запрос идёт туда,
  иначе — generic UI Automation путь.

## Модули и порядок выполнения

### M0 — скелет Core
- Глобальный хоткей (`WH_KEYBOARD_LL`), без отдельного потока —
  message loop уже есть в `WinMain`.
- Layered/topmost/click-through/no-activate overlay-окно на весь
  virtual screen (все мониторы).
- Рисование статичных демо-хинтов (заглушка вместо реальных
  данных от адаптеров/UIA).
- Esc закрывает оверлей без действия.
- Рендер — обычный GDI с `LWA_COLORKEY` (бинарная прозрачность, без
  альфа-блендинга) — достаточно для проверки механики hook/overlay/
  click-through/DPI. Полупрозрачность и анти-алиасинг — отдельная
  задача позже (см. M6/темизация).

### M1 — generic UI Automation fallback
- `IUIAutomation`, обход foreground-окна с условием на
  `InvokePattern`/`TogglePattern`/`SelectionItemPattern`/
  `ExpandCollapsePattern` либо нужный `ControlType`.
- Лимит глубины/таймаут обхода (деревья Chromium/Electron большие).
- `BoundingRectangle` → экранные координаты хинта.
- Клик — `SendInput` по центру rect, т.к. адаптера нет.
- Проверка на Claude Desktop и паре обычных приложений (Notepad,
  Explorer).

### M2 — IPC-сервер в Core
- Локальный WebSocket-сервер (готовая header-only библиотека, не
  писать RFC6455 руками), токен-авторизация.
- Реестр подключений по полю `app` из `hello`.
- Таймаут ожидания `hints` от адаптера (150–250 мс), иначе —
  fallback на generic UIA даже для "умной" цели.

### M3 — браузерный адаптер (расширение MV3)
- Background service worker держит WS-клиент к Core.
- Content script собирает `a`, `button`, `[role=button]`, `input`,
  `[onclick]`, элементы с `tabindex`, с проверкой видимости.
- Перевод координат через `window.screenX/screenY` + поправка на
  высоту UI браузера.
- `activate` — `element.click()`/`element.focus()` в content script,
  без синтетического OS-клика.

### M4 — Rider adapter (плагин IntelliJ Platform, Kotlin)
- `ProjectActivity` поднимает WS-клиент при старте проекта.
- `get_hints` собирает: intention actions в текущей позиции курсора,
  gutter-иконки (`LineMarkerProvider`), breadcrumbs, таб-бар, кнопки
  тулбара — через их Swing-компоненты (`getLocationOnScreen`) и
  `editor.logicalPositionToXY`.
- `activate` — программный вызов `IntentionAction`/`AnAction`, а не
  симуляция клика.

### Проверка Claude Desktop
- Отдельного адаптера нет — чисто generic UIA (M1/M2). Прогнать и
  явно зафиксировать, каких элементов не хватает.

### M5 — эксперимент с VS Code (риск, не гарантирован)
- Extension host не имеет прямого доступа к DOM/пикселям своего UI
  (в отличие от content script на обычной странице и Swing-компонента
  в Rider). Варианты: подключение через Chrome DevTools Protocol
  (`--remote-debugging-port`) или грубая оценка координат через
  метрики строк/`visibleRanges`. Решение go/no-go по итогам
  эксперимента; до этого — VS Code покрывается только generic UIA.

### M6 — Settings window + пресеты тем
- Tray-иконка (`Shell_NotifyIcon`) с меню "Settings…" / "Reload
  adapters" / "Exit".
- Native Win32-диалог: выбор пресета (Dark/Light/Minimal/High
  Contrast/Nyxt-yellow) + несколько override-слайдеров (акцент,
  прозрачность, размер шрифта, радиус скругления) + preview-панель,
  рисующая тот же код, что и реальный оверлей.
- Пресеты — таблица `Theme`-структур в коде, не произвольный
  стайлинг (CSS/WebView2-вариант рассмотрен и отклонён — слишком
  тяжело для той ценности, которую даёт).
- Функция отрисовки хинта параметризована `Theme` с самого M0, чтобы
  не переписывать рендер заново на этом шаге.
- Сохранение выбора в `%APPDATA%\rout\settings.json`.

## Backlog (низкий приоритет)

- CV/OCR fallback (OpenCV + Tesseract) для приложений без адаптера и
  без вменяемого UIA-дерева (игры, canvas, видео). Делать только если
  реально понадобится, после того как всё выше стабильно.

## Правила процесса

- PR/коммиты — небольшие, по возможности не больше ~200 строк диффа.
- Каждый milestone — отдельный(е) коммит(ы), не смешивать с другими.

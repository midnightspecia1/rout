<p align="center">
  <img src="assets/logo-readme.png" alt="rout" width="160">
</p>

<p align="center">
  Keyboard hints for the whole Windows desktop: press a hotkey, type the
  letters on a label, and rout clicks, focuses or scrolls that element.
</p>

## Features

- **Hints anywhere.** Labels over the clickable elements of the active
  window, found through UI Automation. Type a label to click it.
- **Search by name.** `/` filters the hints by the element's name; Enter
  picks a single match.
- **Scroll mode.** Emacs-style keys scroll the chosen region without
  touching the mouse.
- **Adapters.** Apps whose UI Automation tree is poor can describe their
  own UI over a local WebSocket. A JetBrains Rider plugin is included.
- **Themes.** Five presets plus accent, opacity, font size and corner
  radius, with a live preview.

## Keys

| Keys | Action |
| --- | --- |
| `Ctrl+Shift+Space` | Show hints (again or `Esc` to close) |
| Letters | Pick a hint; `Backspace` erases a letter |
| `/` | Search hints by name |
| `Ctrl+Alt+Space` | Scroll mode on the focused region |
| `Ctrl+N` / `Ctrl+P` | Scroll a line down / up |
| `Ctrl+V` / `Alt+V` | Scroll a page down / up |
| `Ctrl+F` / `Ctrl+B` | Scroll right / left |
| `Ctrl+Shift+Space` in scroll mode | Hints inside the region |
| `Esc` / `Ctrl+G` | Leave scroll mode |

Settings open from the tray icon.

## Building

Windows 10 or later. Built with MSYS2 UCRT64 (GCC) and CMake + Ninja:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure fetches IXWebSocket and nlohmann/json.

### Rider adapter

`adapters/rider` is an IntelliJ Platform plugin built against a local Rider
install (`riderPath` in `gradle.properties`), with Rider's bundled JBR:

```sh
cd adapters/rider
set JAVA_HOME=<Rider>\jbr
gradlew buildPlugin
```

Install `build/distributions/rout-rider-*.zip` with
Settings → Plugins → Install Plugin from Disk.

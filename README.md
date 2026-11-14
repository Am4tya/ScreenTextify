# ScreenTextify Studio — Selectext Alternative for Desktop (C++20 / Direct2D / WinRT OCR)

**ScreenTextify Studio** is a high-performance, 100% free and offline native C++20 Windows desktop application designed to surpass the **Selectext** browser extension. It lets you interactively select and copy text or indented programming code directly from any video player, live stream, coding tutorial, Zoom meeting, image, PDF, or screen region.

---

## Key Features

- **Modern Hardware-Accelerated Direct2D Dark-Mode Studio GUI**:
  - Windows 11 Immersive Dark Titlebar & rounded corners (`dwmapi`) with custom Direct2D cards, animated pill switches, live OCR latency metrics, and a scrollable line-numbered **Code & Text Inspector** pane.
- **3 Capture Modes**:
  1. **Interactive Region Selectext (`Alt + Shift + S`)**: Freeze any video/image region and click or drag over highlighted word/line boxes to copy on mouse release.
  2. **Instant Full-Screen Selectext (`Alt + Shift + D`)**: Immediately freezes and detects all selectable text across the active monitor.
  3. **Quick Snip & Instant Copy (`Alt + Shift + C`)**: Drag a rectangle over any text/code to immediately OCR and copy all of it in a single step.
- **Multi-Pass Code-Aware OCR Pipeline (`Windows.Media.Ocr` + Custom CV/DSA)**:
  - **Horizontal-Strip 25th-Percentile Background Estimator**: Eliminates editor selection highlights (e.g., VS Code `#264F78` blue selection boxes), diff lines, and current-line highlights with zero vertical bleed across line boundaries.
  - **Multi-Hue Syntax Contrast Equalization & S-Curve Mapping**: Equalizes multi-color syntax tokens (`Message.findOne(message?._id)`) into crisp ink without thickening ClearType inter-letter valleys (`3x` adaptive super-resolution upscaling).
  - **8-Connected Component Topological Symbol Recovery**: Uses BFS connected-component labeling, curvature center-of-mass (`topMeanX`, `midMeanX`, `botMeanX`), and a quote/parenthesis parity state machine to recover standalone braces (`{`, `}`, `};`), trailing semicolons (`;`), underscores (`_`), quotes (`""`), parentheses (`()`), and `+=` operators.
- **Smart Code Indentation Reconstruction**:
  - Clusters words and vertically overlapping punctuation into visual lines, calculates median character width, normalizes monospace slashed zeros (`ø` → `0`), glues split sub-tokens across `.`, `?.`, `(`, `)`, `;`, `,`, and reconstructs 2/4/8-space code indentation and blank lines.

---

## Global Hotkeys & Overlay Shortcuts

| Shortcut | Action |
| :--- | :--- |
| **`Alt + Shift + S`** | **Interactive Region Selectext**: Crop video/image → click or drag highlighted words |
| **`Alt + Shift + D`** | **Instant Full-Screen Selectext**: Scan active monitor immediately |
| **`Alt + Shift + C`** | **Quick Snip & Instant Copy**: Drag rectangle → auto-copy all text inside & close |
| **`Alt + Shift + H`** | **Open Studio Dashboard**: View history, toggle settings, or save snippets to file |
| **`C` (in Overlay)** | Toggle **Code Indentation Mode** (`ON` / `OFF`) |
| **`L` (in Overlay)** | Switch between **Word Selection** and **Full-Line Selection** |
| **`Ctrl + A` (in Overlay)** | Select and copy **All** detected text in the region |
| **`R` (in Overlay)** | Re-crop a different region on the frozen screen |
| **`Esc` / Right-Click** | Close the overlay |

---

## Building & Running

Run the build script (uses Visual Studio 2022 `cl.exe` + CMake + Ninja with `/O2` optimization):
```bat
build.bat
```

Launch the executable:
```powershell
.\build\ScreenTextify.exe
```

Run the automated GUI & OCR verification suite:
```powershell
.\build\ScreenTextify.exe --self-test | Out-String
```

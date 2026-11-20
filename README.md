# ScreenTextify Studio v3.0 — Selectext Alternative for Desktop (C++20 / Direct2D / PP-OCRv4 + WinRT OCR)

**ScreenTextify Studio** is a high-performance, 100% free and offline native C++20 Windows desktop application designed to surpass the **Selectext** browser extension. It lets you interactively select and copy text or indented programming code directly from any video player, live stream, coding tutorial, Zoom meeting, image, PDF, or screen region.

---

## Key Features

- **Modern Hardware-Accelerated Direct2D Dark-Mode Studio GUI**:
  - Windows 11 Immersive Dark Titlebar & rounded corners (`dwmapi`) with custom Direct2D cards, animated pill switches, live OCR latency metrics, and a scrollable line-numbered **Code & Text Inspector** pane.
- **4 Capture Modes + Image File / Clipboard OCR**:
  1. **Interactive Region Selectext (`Alt + Shift + S`)**: Freeze any video/image region and click or drag over highlighted word/line boxes to copy on mouse release.
  2. **Instant Full-Screen Selectext (`Alt + Shift + D`)**: Immediately freezes and detects all selectable text across the active monitor.
  3. **Quick Snip & Instant Copy (`Alt + Shift + C`)**: Drag a rectangle over any text/code to immediately OCR and copy all of it in a single step.
  4. **Multi-Frame Stitch & Append (`Alt + Shift + A` or `A` in Overlay)**: Scroll through long code files in YouTube videos, capture each screen, and automatically merge & deduplicate overlapping lines onto your clipboard.
  5. **Drag-and-Drop & Clipboard Paste OCR (`Ctrl + V`)**: Drop any PNG/JPG/BMP onto the Studio window or press `Ctrl + V` to OCR a screenshot directly from your clipboard.
- **Persistent Settings, History & Start with Windows**:
  - Automatically saves your toggle preferences and capture history (up to 30 snippets) to `%APPDATA%\ScreenTextify\`, with a one-click **Start with Windows (System Tray)** switch.
- **PP-OCRv5 + v4 Dual-View Code Engine (`PaddleOcr` on ONNX Runtime + `Windows.Media.Ocr` Fallback)**:
  - **Horizontal-Strip 25th-Percentile Background Estimator**: Eliminates editor selection highlights (e.g., VS Code `#264F78` blue selection boxes), diff lines, and dimmed video overlays with zero vertical bleed across line boundaries.
  - **2D 8-Connected Component Line & Clause Detector (~0.3 ms)**: Finds exact pixel-tight bounding boxes of every code line and standalone symbol (`{`, `}`, `};`, `(`, `)`), avoiding the ~500 ms overhead and symbol-dropping of scene-photo text detectors.
  - **ASCII-Constrained CTC Decoding & Word-Level Confidence Ensemble (`rec_en_v5.onnx` + `rec_en.onnx`)**: Combines PP-OCRv5's digit/symbol recognition head (including dotted `0` in `Cascadia Code`) with PP-OCRv4's camelCase retention (`useState`, `useLoginStore`) and a secondary polarity-normalized raw view.
  - **Keyword-Free 2-Bar `=` Verifier & Pushdown Automaton (PDA) Delimiter Stack**: Distinguishes two stacked horizontal bars (`=`) from `-` via connected-component topology, tracks open `{` and `(` delimiters across lines to disambiguate standalone closing `)` vs `}`, and recovers CTC-collapsed empty strings (`""`).
- **Smart Code Indentation Reconstruction**:
  - Clusters words and vertically overlapping punctuation into visual lines, calculates median character width, normalizes monospace slashed zeros (`ø` → `0`), glues split sub-tokens, and reconstructs 2/4/8-space code indentation and blank lines.

---

## Global Hotkeys & Overlay Shortcuts

| Shortcut | Action |
| :--- | :--- |
| **`Alt + Shift + S`** | **Interactive Region Selectext**: Crop video/image → click or drag highlighted words |
| **`Alt + Shift + D`** | **Instant Full-Screen Selectext**: Scan active monitor immediately |
| **`Alt + Shift + C`** | **Quick Snip & Instant Copy**: Drag rectangle → auto-copy all text inside & close |
| **`Alt + Shift + A`** | **Stitch & Append Mode**: Drag over scrolled video code → append & deduplicate onto clipboard |
| **`Alt + Shift + H`** | **Open Studio Dashboard**: View history, toggle settings, or save snippets to file |
| **`Ctrl + V` (in Studio)** | **Paste Image OCR**: OCR a screenshot or copied image file directly from the clipboard |
| **`A` / `Ctrl+Shift+C` (in Overlay)** | **Stitch & Append** current selection onto existing clipboard text |
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

Run the held-out multi-language degraded YouTube video benchmark:
```powershell
.\build\ScreenTextify.exe --benchmark | Out-String
```

Run CLI OCR on any image file (or crop `[x y w h]`):
```powershell
.\build\ScreenTextify.exe --ocr path\to\image.png
```

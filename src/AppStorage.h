#pragma once

#include "Types.h"
#include <string>
#include <vector>

namespace ScreenTextify {

class AppStorage {
public:
    // Returns %APPDATA%\ScreenTextify (creating the directory if needed).
    static std::wstring GetDataDirectory();

    // Loads/saves user settings to %APPDATA%\ScreenTextify\settings.ini.
    static bool LoadSettings(AppSettings& settings);
    static bool SaveSettings(const AppSettings& settings);

    // Loads/saves capture history (up to 30 items) to %APPDATA%\ScreenTextify\history.dat.
    static bool LoadHistory(std::vector<CaptureHistoryItem>& history);
    static bool SaveHistory(const std::vector<CaptureHistoryItem>& history);

    // Queries/updates HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ScreenTextify.
    static bool IsStartWithWindowsEnabled();
    static bool SetStartWithWindows(bool enable);
};

} // namespace ScreenTextify

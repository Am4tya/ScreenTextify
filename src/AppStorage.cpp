#include "AppStorage.h"

#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace ScreenTextify {

namespace {

namespace fs = std::filesystem;

constexpr wchar_t RUN_KEY_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RUN_VALUE_NAME[] = L"ScreenTextify";

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::wstring GetCurrentExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return (len > 0) ? std::wstring(buf, len) : L"";
}

} // namespace

std::wstring AppStorage::GetDataDirectory() {
    PWSTR appDataPath = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath) {
        dir = fs::path(appDataPath) / L"ScreenTextify";
        CoTaskMemFree(appDataPath);
    } else {
        wchar_t envBuf[MAX_PATH * 2] = {};
        DWORD n = GetEnvironmentVariableW(L"APPDATA", envBuf, static_cast<DWORD>(std::size(envBuf)));
        if (n > 0) {
            dir = fs::path(envBuf) / L"ScreenTextify";
        } else {
            dir = fs::path(GetCurrentExePath()).parent_path() / L"data";
        }
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.wstring();
}

bool AppStorage::LoadSettings(AppSettings& settings) {
    settings.startWithWindows = IsStartWithWindowsEnabled();

    fs::path path = fs::path(GetDataDirectory()) / L"settings.ini";
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        bool bval = (val == "1" || val == "true");

        if (key == "codeIndentationMode") settings.codeIndentationMode = bval;
        else if (key == "lineSelectionMode") settings.lineSelectionMode = bval;
        else if (key == "autoCopyOnRelease") settings.autoCopyOnRelease = bval;
        else if (key == "autoCloseAfterCopy") settings.autoCloseAfterCopy = bval;
        else if (key == "upscaleForOcr") settings.upscaleForOcr = bval;
        else if (key == "showLivePreviewCard") settings.showLivePreviewCard = bval;
    }
    return true;
}

bool AppStorage::SaveSettings(const AppSettings& settings) {
    fs::path path = fs::path(GetDataDirectory()) / L"settings.ini";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out << "codeIndentationMode=" << (settings.codeIndentationMode ? 1 : 0) << "\n";
    out << "lineSelectionMode=" << (settings.lineSelectionMode ? 1 : 0) << "\n";
    out << "autoCopyOnRelease=" << (settings.autoCopyOnRelease ? 1 : 0) << "\n";
    out << "autoCloseAfterCopy=" << (settings.autoCloseAfterCopy ? 1 : 0) << "\n";
    out << "upscaleForOcr=" << (settings.upscaleForOcr ? 1 : 0) << "\n";
    out << "showLivePreviewCard=" << (settings.showLivePreviewCard ? 1 : 0) << "\n";
    out << "startWithWindows=" << (settings.startWithWindows ? 1 : 0) << "\n";
    return out.good();
}

bool AppStorage::LoadHistory(std::vector<CaptureHistoryItem>& history) {
    history.clear();
    fs::path path = fs::path(GetDataDirectory()) / L"history.dat";
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    char magic[4] = {};
    in.read(magic, 4);
    if (!in || magic[0] != 'S' || magic[1] != 'T' || magic[2] != 'H' || magic[3] != '1') {
        return false;
    }

    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || count > 64) return false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t tsLen = 0, textLen = 0;
        uint8_t wasCode = 1;
        uint64_t wc = 0;
        double ms = 0.0;

        in.read(reinterpret_cast<char*>(&tsLen), sizeof(tsLen));
        in.read(reinterpret_cast<char*>(&textLen), sizeof(textLen));
        in.read(reinterpret_cast<char*>(&wasCode), sizeof(wasCode));
        in.read(reinterpret_cast<char*>(&wc), sizeof(wc));
        in.read(reinterpret_cast<char*>(&ms), sizeof(ms));
        if (!in || tsLen > 256 || textLen > 2 * 1024 * 1024) break;

        std::string tsUtf8(tsLen, '\0');
        std::string textUtf8(textLen, '\0');
        if (tsLen > 0) in.read(tsUtf8.data(), static_cast<std::streamsize>(tsLen));
        if (textLen > 0) in.read(textUtf8.data(), static_cast<std::streamsize>(textLen));
        if (!in) break;

        CaptureHistoryItem item;
        item.timestamp = Utf8ToWide(tsUtf8);
        item.text = Utf8ToWide(textUtf8);
        item.wasCodeMode = (wasCode != 0);
        item.wordCount = static_cast<size_t>(wc);
        item.ocrTimeMs = ms;
        history.push_back(std::move(item));
    }

    return !history.empty();
}

bool AppStorage::SaveHistory(const std::vector<CaptureHistoryItem>& history) {
    fs::path path = fs::path(GetDataDirectory()) / L"history.dat";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    const char magic[4] = { 'S', 'T', 'H', '1' };
    out.write(magic, 4);

    uint32_t count = static_cast<uint32_t>(std::min<size_t>(history.size(), 30));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (uint32_t i = 0; i < count; ++i) {
        const auto& item = history[i];
        std::string tsUtf8 = WideToUtf8(item.timestamp);
        std::string textUtf8 = WideToUtf8(item.text);
        uint32_t tsLen = static_cast<uint32_t>(tsUtf8.size());
        uint32_t textLen = static_cast<uint32_t>(textUtf8.size());
        uint8_t wasCode = item.wasCodeMode ? 1 : 0;
        uint64_t wc = static_cast<uint64_t>(item.wordCount);
        double ms = item.ocrTimeMs;

        out.write(reinterpret_cast<const char*>(&tsLen), sizeof(tsLen));
        out.write(reinterpret_cast<const char*>(&textLen), sizeof(textLen));
        out.write(reinterpret_cast<const char*>(&wasCode), sizeof(wasCode));
        out.write(reinterpret_cast<const char*>(&wc), sizeof(wc));
        out.write(reinterpret_cast<const char*>(&ms), sizeof(ms));
        if (tsLen > 0) out.write(tsUtf8.data(), static_cast<std::streamsize>(tsLen));
        if (textLen > 0) out.write(textUtf8.data(), static_cast<std::streamsize>(textLen));
    }
    return out.good();
}

bool AppStorage::IsStartWithWindowsEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH * 2] = {};
    DWORD type = 0;
    DWORD size = sizeof(val);
    LONG res = RegQueryValueExW(hKey, RUN_VALUE_NAME, nullptr, &type, reinterpret_cast<LPBYTE>(val), &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS && type == REG_SZ && val[0] != L'\0');
}

bool AppStorage::SetStartWithWindows(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG res = ERROR_SUCCESS;
    if (enable) {
        std::wstring exe = GetCurrentExePath();
        if (exe.empty()) {
            RegCloseKey(hKey);
            return false;
        }
        std::wstring cmd = L"\"" + exe + L"\" --tray";
        DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        res = RegSetValueExW(hKey, RUN_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
    } else {
        res = RegDeleteValueW(hKey, RUN_VALUE_NAME);
        if (res == ERROR_FILE_NOT_FOUND) res = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

} // namespace ScreenTextify

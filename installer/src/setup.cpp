// Blood Dragon VR Setup: a text-mode (80s DOS style) installer for the
// Far Cry 3 Blood Dragon VR mod. One exe, the mod files are resources inside it.
//
// Interactive:  BloodDragonVR-Setup.exe
// Uninstall:    BloodDragonVR-Setup.exe /uninstall            (from Windows' Apps list)
// Scripted:     BloodDragonVR-Setup.exe /install   "<game folder>" [/nolaa] [/nolock] [/noshortcut] [/resetini] [/dinput8|/version]
//               BloodDragonVR-Setup.exe /uninstall "<game folder>" [/removeini] [/keeplaa] [/keeplock]
// Scripted runs print what they do and return 0 on success.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include "resource.h"

#ifndef BDVR_BUILD
#define BDVR_BUILD "dev"
#endif
#define WIDE2(x) L##x
#define WIDE(x) WIDE2(x)
static const wchar_t* kBuild = WIDE(BDVR_BUILD);

using std::wstring;
using std::vector;

static const wchar_t* kExeName = L"fc3_blooddragon_d3d11.exe";
static const wchar_t* kGameFolder = L"Far Cry 3 Blood Dragon";
static const wchar_t* kModFolder = L"BloodDragonVR";
static const wchar_t* kSetupName = L"BloodDragonVR-Setup.exe";
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BloodDragonVR";
static const wchar_t* kShortcutName = L"Blood Dragon VR.lnk";
static const wchar_t* kOldGuideName = L"Blood Dragon VR controls.html";  // v1 installers wrote this; removed now
static const wchar_t* kPdfName = L"Blood Dragon VR controls.pdf";  // the controls pictures as one PDF
static const wchar_t* kPromptsName = L"bdvr-prompts.txt";  // prompts1: bin\\bdvr-prompts.txt
static const wchar_t* kPicFolder = L"bdvr-pictures";  // bin\bdvr-pictures: the menu's font and controls pictures
// The Steam copy the mod was made for (FC3_d3d11.dll from the 2019 update). Other copies get a warning, not a block.
static const DWORD kEngineDllSize = 28693672;

// ---------------------------------------------------------------- helpers

static wstring lower(wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}
static bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static wstring parentDir(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? L"" : p.substr(0, i);
}
static wstring fileName(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}
static wstring fullPath(const wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    return (n && n < MAX_PATH * 2) ? wstring(buf) : p;
}
static wstring stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
static wstring errText(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    wchar_t b[32];
    swprintf(b, 32, L" (error %lu)", e);
    return s + b;
}
static bool readFile(const wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}
static bool writeFile(const wstring& p, const void* data, size_t n, wstring* err) {
    // write beside the target, then swap it in, then read it back to check
    wstring tmp = p + L".bdvr-new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = errText(GetLastError()); return false; }
    DWORD put = 0;
    bool ok = WriteFile(h, data, (DWORD)n, &put, nullptr) && put == n;
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); if (err) *err = errText(e); return false; }
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        e = GetLastError();
        DeleteFileW(tmp.c_str());
        if (err) *err = errText(e);
        return false;
    }
    std::string back;
    if (!readFile(p, back) || back.size() != n || memcmp(back.data(), data, n) != 0) {
        if (err) *err = L"the file did not read back the same";
        return false;
    }
    return true;
}
static bool resource(int id, const void** data, size_t* n) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    *data = LockResource(g);
    *n = SizeofResource(nullptr, r);
    return *data && *n;
}
static std::string resourceString(int id) {
    const void* d;
    size_t n;
    if (!resource(id, &d, &n)) return {};
    return std::string((const char*)d, n);
}
// The pictures inside this exe: resource IDR_PICINDEX lists their file names, one per line, and picture i is
// resource IDR_PIC0 + i. build.sh makes both from the installer's pictures folder.
struct Pic { wstring name; const void* data; size_t n; };
static wstring utf8(const std::string& s);
static vector<Pic> pictures() {
    vector<Pic> out;
    std::string idx = resourceString(IDR_PICINDEX);
    size_t p = 0;
    for (int i = 0; p < idx.size(); i++) {
        size_t e = idx.find('\n', p);
        if (e == std::string::npos) e = idx.size();
        std::string name = idx.substr(p, e - p);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        p = e + 1;
        if (name.empty()) { i--; continue; }
        Pic pic{utf8(name), nullptr, 0};
        if (resource(IDR_PIC0 + i, &pic.data, &pic.n)) out.push_back(pic);
    }
    return out;
}

static wstring utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static wstring regString(HKEY root, const wchar_t* key, const wchar_t* val, REGSAM view) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[1024];
    DWORD type = 0, sz = sizeof(buf) - sizeof(wchar_t);
    wstring out;
    if (RegQueryValueExW(k, val, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[sz / sizeof(wchar_t)] = 0;
        out = buf;
    }
    RegCloseKey(k);
    return out;
}
static wstring slashes(wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

// ---------------------------------------------------------------- game folder

static wstring binDir(const wstring& g) { return g + L"\\bin"; }
static wstring gameExe(const wstring& g) { return binDir(g) + L"\\" + kExeName; }
static wstring modDir(const wstring& g) { return g + L"\\" + kModFolder; }
static wstring stateFile(const wstring& g) { return modDir(g) + L"\\install.ini"; }
static bool isGameDir(const wstring& g) { return !g.empty() && fileExists(gameExe(g)); }
static wstring picDir(const wstring& g) { return binDir(g) + L"\\" + kPicFolder; }
// Size of the game's engine dll, or 0 if it can't be read.
static DWORD engineDllSize(const wstring& g) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW((binDir(g) + L"\\FC3_d3d11.dll").c_str(), GetFileExInfoStandard, &a)) return 0;
    return a.nFileSizeLow;
}
static bool knownGameVersion(const wstring& g) { return engineDllSize(g) == kEngineDllSize; }
static bool isOurDll(const wstring& p);
// loader1 (GitHub #1, 23 Nov): the mod can load as version.dll (the default) or dinput8.dll. Players who
// fix the game's many-thread CPU crash with a Windows compatibility database (ProcessorCountLie) need
// dinput8.dll: that fix makes Windows load its own version.dll first, so ours is never used.
static const wchar_t* kLoaders[2] = {L"version.dll", L"dinput8.dll"};
static bool modDllIn(const wstring& g) {
    for (auto n : kLoaders) if (isOurDll(binDir(g) + L"\\" + n)) return true;
    return false;
}
// A Windows compatibility database (sdbinst) aimed at the game's exe, which is how the CPU-count fix is
// applied. loader2 (23 Nov): compatibility modes / settings from the exe's Properties (AppCompatFlags\\Layers)
// no longer count: they are common (DPI, full-screen optimisations), the mod's version.dll works with them,
// and counting them switched DrGonzaga's install to dinput8.dll for no reason.
static bool compatFixFound(const wstring& g) {
    (void)g;
    for (REGSAM v : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Custom\\") + kExeName).c_str(),
                          0, KEY_READ | v, &k) == ERROR_SUCCESS) {
            RegCloseKey(k);
            return true;
        }
    }
    return false;
}
// The loader in use: what install.ini says, else whichever of ours is in bin, else the default.
static int currentLoader(const wstring& g);

// Accepts the game folder, its bin folder or the exe itself, typed with or without quotes.
static wstring normaliseGameDir(wstring d) {
    while (!d.empty() && (d.back() == L' ' || d.back() == L'"')) d.pop_back();
    while (!d.empty() && (d.front() == L' ' || d.front() == L'"')) d.erase(0, 1);
    d = slashes(d);
    if (d.empty()) return d;
    d = fullPath(d);
    if (lower(fileName(d)) == lower(kExeName)) d = parentDir(d);
    if (lower(fileName(d)) == L"bin" && isGameDir(parentDir(d))) d = parentDir(d);
    return d;
}

static void addSteamLibrary(vector<wstring>& libs, const wstring& root) {
    if (root.empty()) return;
    libs.push_back(slashes(root));
    for (const wchar_t* rel : {L"\\steamapps\\libraryfolders.vdf", L"\\config\\libraryfolders.vdf"}) {
        std::string raw;
        if (!readFile(slashes(root) + rel, raw)) continue;
        wstring t = utf8(raw);
        size_t pos = 0;
        while ((pos = t.find(L"\"path\"", pos)) != wstring::npos) {
            pos += 6;
            size_t a = t.find(L'"', pos);
            if (a == wstring::npos) break;
            wstring v;
            size_t i = a + 1;
            for (; i < t.size() && t[i] != L'"' && t[i] != L'\n'; i++) {
                if (t[i] == L'\\' && i + 1 < t.size()) i++;  // vdf escapes: \\ -> '\'
                v += t[i];
            }
            pos = i;
            if (!v.empty()) libs.push_back(slashes(v));
        }
    }
}

static bool steamOwns(const wstring& g);

static vector<wstring> findGames() {
    vector<wstring> libs, found;
    addSteamLibrary(libs, regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0));
    addSteamLibrary(libs, regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY));
    addSteamLibrary(libs, regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY));
    vector<wstring> cands;
    for (auto& l : libs) cands.push_back(l + L"\\steamapps\\common\\" + kGameFolder);
    for (REGSAM v : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        cands.push_back(regString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 233270", L"InstallLocation", v));
    }
    // ubi1 (beta feedback 23 Nov): Ubisoft Connect's own list of installed games
    for (REGSAM v : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Ubisoft\\Launcher\\Installs", 0, KEY_READ | v, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[64];
        for (DWORD i = 0; i < 2000; i++) {
            DWORD n = 64;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wstring d = regString(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\Ubisoft\\Launcher\\Installs\\" + wstring(sub)).c_str(), L"InstallDir", v);
            if (!d.empty()) cands.push_back(d);
        }
        RegCloseKey(k);
    }
    DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        wstring d = wstring(1, (wchar_t)(L'A' + i)) + L":";
        if (GetDriveTypeW((d + L"\\").c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* s : {L"\\SteamLibrary", L"\\Steam", L"\\Games\\Steam", L"\\Games\\SteamLibrary",
                                 L"\\Program Files (x86)\\Steam", L"\\Program Files\\Steam"})
            cands.push_back(d + s + L"\\steamapps\\common\\" + kGameFolder);
        cands.push_back(d + L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\games\\" + kGameFolder);
    }
    vector<wstring> seen;
    for (auto& c : cands) {
        if (c.empty()) continue;
        wstring g = normaliseGameDir(c);
        if (!isGameDir(g)) continue;
        wstring k = lower(g);
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
        seen.push_back(k);
        found.push_back(g);
    }
    // the copy Steam says is installed comes first (a leftover folder from a moved or old install
    // has no Steam manifest beside it), then a copy that already has the mod
    auto rank = [](const wstring& g) {
        int r = steamOwns(g) ? 0 : 2;
        if (!fileExists(stateFile(g)) && !modDllIn(g)) r++;
        return r;
    };
    std::stable_sort(found.begin(), found.end(), [&](const wstring& x, const wstring& y) { return rank(x) < rank(y); });
    return found;
}

// Steam's update file for the game: <library>\steamapps\appmanifest_<id>.acf
static wstring steamManifest(const wstring& g) {
    wstring common = parentDir(g), steamapps = parentDir(common);
    if (lower(fileName(common)) != L"common" || lower(fileName(steamapps)) != L"steamapps") return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((steamapps + L"\\appmanifest_*.acf").c_str(), &fd);
    wstring hit;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string raw;
            wstring p = steamapps + L"\\" + fd.cFileName;
            if (readFile(p, raw) && lower(utf8(raw)).find(lower(L"\"" + wstring(kGameFolder) + L"\"")) != wstring::npos) {
                hit = p;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (hit.empty() && fileExists(steamapps + L"\\appmanifest_233270.acf")) hit = steamapps + L"\\appmanifest_233270.acf";
    return hit;
}
// True when this folder is the one Steam has registered: its library holds the game's manifest
// and the manifest's installdir names this folder.
static bool steamOwns(const wstring& g) {
    wstring m = steamManifest(g);
    if (m.empty()) return false;
    std::string raw;
    if (!readFile(m, raw)) return false;
    return lower(utf8(raw)).find(L"\"" + lower(fileName(g)) + L"\"") != wstring::npos;
}
static bool isReadOnly(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}
static bool setReadOnly(const wstring& p, bool on) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    a = on ? (a | FILE_ATTRIBUTE_READONLY) : (a & ~FILE_ATTRIBUTE_READONLY);
    if (a == 0) a = FILE_ATTRIBUTE_NORMAL;
    return SetFileAttributesW(p.c_str(), a);
}

static bool gameRunning() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool hit = false;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (lower(pe.szExeFile) == lower(kExeName)) { hit = true; break; }
    CloseHandle(s);
    return hit;
}

// The 4 GB patch is one flag in the exe's header (IMAGE_FILE_LARGE_ADDRESS_AWARE).
// op 0 = read, 1 = set, 2 = clear. Returns 1 set, 0 clear, -1 error.
static int largeAddress(const wstring& exe, int op, wstring* err) {
    if (op) SetFileAttributesW(exe.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(exe.c_str(), GENERIC_READ | (op ? GENERIC_WRITE : 0), FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = errText(GetLastError()); return -1; }
    unsigned char hdr[1024];
    DWORD got = 0;
    int result = -1;
    if (ReadFile(h, hdr, sizeof(hdr), &got, nullptr) && got >= 0x40 && hdr[0] == 'M' && hdr[1] == 'Z') {
        DWORD pe = hdr[0x3c] | hdr[0x3d] << 8 | hdr[0x3e] << 16 | (DWORD)hdr[0x3f] << 24;
        DWORD at = pe + 4 + 18;  // FILE_HEADER.Characteristics
        if (pe + 24 <= got && memcmp(hdr + pe, "PE\0\0", 4) == 0) {
            WORD ch = hdr[at] | hdr[at + 1] << 8;
            WORD want = op == 1 ? (ch | IMAGE_FILE_LARGE_ADDRESS_AWARE)
                      : op == 2 ? (ch & ~IMAGE_FILE_LARGE_ADDRESS_AWARE) : ch;
            if (want != ch) {
                unsigned char b[2] = {(unsigned char)(want & 0xff), (unsigned char)(want >> 8)};
                DWORD put = 0;
                if (SetFilePointer(h, at, nullptr, FILE_BEGIN) == at && WriteFile(h, b, 2, &put, nullptr) && put == 2)
                    result = (want & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? 1 : 0;
                else if (err) *err = errText(GetLastError());
            } else {
                result = (ch & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? 1 : 0;
            }
        } else if (err) *err = L"not a Windows program header";
    } else if (err) *err = L"could not read the program header";
    CloseHandle(h);
    return result;
}

static wstring desktopShortcut() {
    wchar_t p[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, p))) return {};
    return wstring(p) + L"\\" + kShortcutName;
}
static bool makeShortcut(const wstring& lnk, const wstring& target, const wstring& workDir, wstring* err) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool inited = SUCCEEDED(hr);
    IShellLinkW* sl = nullptr;
    bool ok = false;
    hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl);
    if (SUCCEEDED(hr)) {
        sl->SetPath(target.c_str());
        sl->SetWorkingDirectory(workDir.c_str());
        sl->SetDescription(L"Far Cry 3 Blood Dragon in VR");
        sl->SetIconLocation(target.c_str(), 0);
        IPersistFile* pf = nullptr;
        hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk.c_str(), TRUE);
            ok = SUCCEEDED(hr);
            pf->Release();
        }
        sl->Release();
    }
    if (!ok && err) { wchar_t b[48]; swprintf(b, 48, L"Windows shortcut error 0x%08lx", (unsigned long)hr); *err = b; }
    if (inited) CoUninitialize();
    return ok;
}

static wstring openXrRuntime() {
    return regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_32KEY);
}

static wstring stateGet(const wstring& g, const wchar_t* key) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"install", key, L"", buf, 1024, stateFile(g).c_str());
    return buf;
}
static void stateSet(const wstring& g, const wchar_t* key, const wstring& v) {
    WritePrivateProfileStringW(L"install", key, v.c_str(), stateFile(g).c_str());
}

// ---------------------------------------------------------------- install / uninstall

struct Step {
    std::function<void(const wstring&)> begin;               // a step starts
    std::function<void(bool ok, const wstring& note)> end;   // it finished
};

struct InstallOptions {
    bool laa = true, steamLock = true, shortcut = true, keepIni = true;
    bool dinput8 = false;  // loader1: install as dinput8.dll instead of version.dll
};
struct UninstallOptions {
    bool keepIni = true, undoLaa = true, unlock = true, removeShortcut = true;
};

// Returns an empty string on success, otherwise what failed.
static wstring doInstall(const wstring& g, const InstallOptions& o, const Step& ui) {
    wstring err;
    const wstring bin = binDir(g), mod = modDir(g), bak = mod + L"\\backup";

    ui.begin(L"Checking the game is closed");
    if (gameRunning()) { ui.end(false, L"running"); return L"The game is running. Close it, then try again."; }
    ui.end(true, L"");

    ui.begin(L"Making the BloodDragonVR folder");
    CreateDirectoryW(mod.c_str(), nullptr);
    CreateDirectoryW(bak.c_str(), nullptr);
    if (!dirExists(bak)) { ui.end(false, L""); return L"Could not make " + bak + L": " + errText(GetLastError()); }
    ui.end(true, L"");

    ui.begin(L"Backing up the original files");
    wstring note;
    if (!fileExists(bak + L"\\" + kExeName)) {
        // keep the cleanest copy of the game's exe we can find
        wstring orig = gameExe(g) + L".original";
        wstring src = fileExists(orig) ? orig : gameExe(g);
        if (!CopyFileW(src.c_str(), (bak + L"\\" + kExeName).c_str(), TRUE)) {
            ui.end(false, L"");
            return L"Could not back up the game's exe: " + errText(GetLastError());
        }
        note = L"game exe";
    }
    for (auto n : kLoaders) {  // another mod's dll of either name is kept (ours never is)
        wstring f = bin + L"\\" + n, b = bak + L"\\" + n;
        if (fileExists(f) && !isOurDll(f) && !fileExists(b)) {
            CopyFileW(f.c_str(), b.c_str(), TRUE);
            note += (note.empty() ? L"" : L", ") + wstring(n);
        }
    }
    if (fileExists(bin + L"\\fc3vr.ini")) {
        CopyFileW((bin + L"\\fc3vr.ini").c_str(), (bak + L"\\fc3vr-" + stamp() + L".ini").c_str(), TRUE);
        note += note.empty() ? L"settings" : L", settings";
    }
    ui.end(true, note.empty() ? L"already done" : note);

    const wstring use = kLoaders[o.dinput8 ? 1 : 0], other = kLoaders[o.dinput8 ? 0 : 1];
    ui.begin(L"Copying the VR mod (" + use + L")");
    const void* dll; size_t dllN;
    if (!resource(IDR_MODDLL, &dll, &dllN)) { ui.end(false, L""); return L"This setup program is damaged (no mod inside)."; }
    if (!writeFile(bin + L"\\" + use, dll, dllN, &err)) {
        ui.end(false, L"");
        return L"Could not write " + use + L": " + err;
    }
    // one active copy only: our dll under the other name goes; another mod's file under that name stays
    // (the mod starts once per game even if both load)
    wstring otherNote;
    if (isOurDll(bin + L"\\" + other)) {
        DeleteFileW((bin + L"\\" + other).c_str());
        // another mod's dll of that name, kept from before us, goes back
        wstring prev = bak + L"\\" + other;
        if (fileExists(prev) && !isOurDll(prev)) CopyFileW(prev.c_str(), (bin + L"\\" + other).c_str(), FALSE);
        otherNote = L"old " + other + L" removed";
    }
    stateSet(g, L"Loader", use);
    ui.end(true, otherNote);

    ui.begin(L"Writing the settings file (fc3vr.ini)");
    const void* ini; size_t iniN;
    if (!resource(IDR_DEFAULTINI, &ini, &iniN)) { ui.end(false, L""); return L"This setup program is damaged (no settings inside)."; }
    writeFile(mod + L"\\fc3vr.default.ini", ini, iniN, nullptr);
    if (o.keepIni && fileExists(bin + L"\\fc3vr.ini")) {
        // autores1 (bd135): v0.69/v0.70 shipped a fixed 3632x3600 render size (blurry on sharper headsets).
        // A kept file still holding exactly that size goes automatic (the headset's recommended size).
        const wstring f = bin + L"\\fc3vr.ini";
        bool oldSize = GetPrivateProfileIntW(L"render", L"Width", 0, f.c_str()) == 3632 &&
                       GetPrivateProfileIntW(L"render", L"Height", 0, f.c_str()) == 3600;
        if (oldSize) {
            WritePrivateProfileStringW(L"render", L"Width", nullptr, f.c_str());
            WritePrivateProfileStringW(L"render", L"Height", nullptr, f.c_str());
        }
        ui.end(true, oldSize ? L"kept yours (render size now automatic)" : L"kept yours");
    } else {
        if (!writeFile(bin + L"\\fc3vr.ini", ini, iniN, &err)) { ui.end(false, L""); return L"Could not write fc3vr.ini: " + err; }
        ui.end(true, L"defaults");
    }

    ui.begin(L"VR button names for the game's prompts (bdvr-prompts.txt)");
    {
        // prompts1: like fc3vr.ini, a player's own edited copy is kept when "Keep my current settings" is ticked
        // (the new one goes beside it in BloodDragonVR); otherwise theirs is backed up first.
        std::string pr = resourceString(IDR_PROMPTS), have;
        const wstring dst = bin + L"\\" + kPromptsName;
        writeFile(mod + L"\\bdvr-prompts.default.txt", pr.data(), pr.size(), nullptr);
        if (readFile(dst, have) && have == pr) {
            ui.end(true, L"up to date");
        } else if (!have.empty() && o.keepIni) {
            ui.end(true, L"kept yours (new one in BloodDragonVR\\bdvr-prompts.default.txt)");
        } else {
            if (!have.empty()) CopyFileW(dst.c_str(), (bak + L"\\bdvr-prompts-" + stamp() + L".txt").c_str(), TRUE);
            if (!writeFile(dst, pr.data(), pr.size(), &err)) { ui.end(false, L""); return L"Could not write " + dst + L": " + err; }
            ui.end(true, have.empty() ? L"" : L"yours backed up");
        }
    }

    ui.begin(L"Khronos OpenXR loader (bin\\openxr_loader.dll, used only with [vr] OpenXrLoader=1)");
    {
        // xrloader2 (v0.72): the 32-bit Khronos loader the mod can use for OpenXR API layers. Another copy already there
        // (another tool's) is left alone; ours is replaced when it changes.
        std::string xl = resourceString(IDR_XRLOADER), have;
        const wstring dst = bin + L"\\openxr_loader.dll";
        const bool there = readFile(dst, have);
        if (xl.empty()) ui.end(true, L"not in this Setup");
        else if (there && have == xl) { stateSet(g, L"XrLoader", L"1"); ui.end(true, L"up to date"); }
        else if (there && stateGet(g, L"XrLoader") != L"1") ui.end(true, L"another copy is there: kept");
        else if (!writeFile(dst, xl.data(), xl.size(), &err)) ui.end(false, L"could not write it (the mod works without it)");
        else { stateSet(g, L"XrLoader", L"1"); ui.end(true, L""); }
    }

    ui.begin(L"Copying the menu font and controls pictures");
    {
        const wstring pd = picDir(g);
        CreateDirectoryW(pd.c_str(), nullptr);
        wstring names, saved;
        int n = 0;
        for (auto& pic : pictures()) {
            wstring dst = pd + L"\\" + pic.name;
            std::string have;
            // a different file of the same name (one you drew yourself?) is backed up before it is replaced
            if (readFile(dst, have) && (have.size() != pic.n || memcmp(have.data(), pic.data, pic.n))) {
                wstring keep = bak + L"\\" + kPicFolder + L"-" + stamp();
                CreateDirectoryW(keep.c_str(), nullptr);
                CopyFileW(dst.c_str(), (keep + L"\\" + pic.name).c_str(), FALSE);
                saved = L", old ones backed up";
            }
            if (!writeFile(dst, pic.data, pic.n, &err)) { ui.end(false, L""); return L"Could not write " + dst + L": " + err; }
            names += (names.empty() ? L"" : L"|") + pic.name;
            n++;
        }
        stateSet(g, L"Pictures", names);
        ui.end(true, std::to_wstring(n) + L" files" + saved);
    }

    ui.begin(L"4 GB memory patch");
    if (o.laa) {
        int was = largeAddress(gameExe(g), 0, &err);
        int now = was == 1 ? 1 : largeAddress(gameExe(g), 1, &err);
        if (now != 1) { ui.end(false, L""); return L"Could not apply the 4 GB patch: " + err; }
        if (was == 0) stateSet(g, L"Laa", L"1");          // we set it, so uninstall may clear it
        else if (stateGet(g, L"Laa").empty()) stateSet(g, L"Laa", L"0");
        ui.end(true, was == 1 ? L"already on" : L"on");
    } else {
        ui.end(true, L"skipped");
    }

    ui.begin(L"Locking Steam updates");
    if (o.steamLock) {
        wstring m = steamManifest(g);
        if (m.empty()) {
            ui.end(true, L"not a Steam copy");
        } else if (isReadOnly(m)) {
            if (stateGet(g, L"SteamLock").empty()) stateSet(g, L"SteamLock", L"");
            ui.end(true, L"already locked");
        } else if (setReadOnly(m, true)) {
            stateSet(g, L"SteamLock", m);
            ui.end(true, L"locked");
        } else {
            ui.end(false, L"skipped");  // not fatal
        }
    } else {
        ui.end(true, L"skipped");
    }

    ui.begin(L"Desktop shortcut");
    if (o.shortcut) {
        wstring lnk = desktopShortcut();
        if (!lnk.empty() && makeShortcut(lnk, gameExe(g), bin, &err)) { stateSet(g, L"Shortcut", lnk); ui.end(true, L""); }
        else ui.end(false, L"skipped");  // not fatal
    } else {
        ui.end(true, L"skipped");
    }

    ui.begin(L"Adding Blood Dragon VR to Windows' app list");
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    wstring setup = mod + L"\\" + kSetupName;
    if (lower(fullPath(self)) != lower(fullPath(setup))) CopyFileW(self, setup.c_str(), FALSE);
    std::string tips = resourceString(IDR_TIPS);
    writeFile(mod + L"\\Setup tips.txt", tips.data(), tips.size(), nullptr);
    std::string pdf = resourceString(IDR_CONTROLSPDF);
    writeFile(mod + L"\\" + kPdfName, pdf.data(), pdf.size(), nullptr);
    DeleteFileW((mod + L"\\" + kOldGuideName).c_str());  // the old html guide (out of date; the in-game menu has the controls now)
    stateSet(g, L"Build", kBuild);
    stateSet(g, L"GameVersion", knownGameVersion(g) ? L"steam-2019" : L"unknown (FC3_d3d11.dll " + std::to_wstring(engineDllSize(g)) + L" bytes)");
    stateSet(g, L"Installed", stamp());
    HKEY k;
    bool regOk = false;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        auto sz = [&](const wchar_t* n, const wstring& v) {
            RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        auto dw = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, 4); };
        sz(L"DisplayName", L"Blood Dragon VR (Far Cry 3 Blood Dragon VR mod)");
        sz(L"DisplayVersion", kBuild);
        sz(L"DisplayIcon", setup);
        sz(L"InstallLocation", g);
        sz(L"UninstallString", L"\"" + setup + L"\" /uninstall");
        dw(L"NoModify", 1);
        dw(L"NoRepair", 1);
        dw(L"EstimatedSize", (DWORD)((dllN + 1023) / 1024));
        RegCloseKey(k);
        regOk = true;
    }
    ui.end(true, regOk ? L"" : L"skipped");
    return L"";
}

static bool isOurDll(const wstring& p) {
    std::string d;
    if (!readFile(p, d)) return false;
    return d.find("fc3vr.ini") != std::string::npos || d.find("f\0c\0""3\0v\0r\0", 0, 10) != std::string::npos;
}

static int currentLoader(const wstring& g) {
    wstring s = lower(stateGet(g, L"Loader"));
    if (s == L"dinput8.dll") return 1;
    if (s == L"version.dll") return 0;
    return isOurDll(binDir(g) + L"\\dinput8.dll") && !isOurDll(binDir(g) + L"\\version.dll") ? 1 : 0;
}

static wstring doUninstall(const wstring& g, const UninstallOptions& o, const Step& ui) {
    const wstring bin = binDir(g), mod = modDir(g), bak = mod + L"\\backup";
    CreateDirectoryW(mod.c_str(), nullptr);
    CreateDirectoryW(bak.c_str(), nullptr);

    ui.begin(L"Checking the game is closed");
    if (gameRunning()) { ui.end(false, L"running"); return L"The game is running. Close it, then try again."; }
    ui.end(true, L"");

    ui.begin(L"Removing the VR mod (version.dll / dinput8.dll)");
    bool restored = false;
    for (auto n : kLoaders) {
        wstring dll = bin + L"\\" + n;
        // version.dll goes whatever it is (as before); dinput8.dll only when it is ours
        if (fileExists(dll) && (wcscmp(n, L"version.dll") == 0 || isOurDll(dll)) && !DeleteFileW(dll.c_str())) {
            DWORD e = GetLastError();
            ui.end(false, L"");
            return L"Could not remove " + wstring(n) + L": " + errText(e);
        }
        // put back another mod's dll of that name if there was one before us
        wstring prev = bak + L"\\" + n;
        if (!fileExists(dll) && fileExists(prev) && !isOurDll(prev) && CopyFileW(prev.c_str(), dll.c_str(), FALSE)) restored = true;
    }
    ui.end(true, restored ? L"previous dll restored" : L"");

    ui.begin(L"Settings file (fc3vr.ini)");
    wstring ini = bin + L"\\fc3vr.ini";
    if (!fileExists(ini)) ui.end(true, L"none");
    else if (o.keepIni) ui.end(true, L"kept");
    else if (MoveFileExW(ini.c_str(), (bak + L"\\fc3vr-" + stamp() + L".ini").c_str(), MOVEFILE_COPY_ALLOWED)) ui.end(true, L"moved to backup");
    else ui.end(false, L"left in place");
    {  // prompts1: the prompts file goes with the settings file
        wstring pf = bin + L"\\" + kPromptsName;
        if (fileExists(pf) && !o.keepIni) MoveFileExW(pf.c_str(), (bak + L"\\bdvr-prompts-" + stamp() + L".txt").c_str(), MOVEFILE_COPY_ALLOWED);
    }
    DeleteFileW((mod + L"\\bdvr-prompts.default.txt").c_str());
    {  // xrloader2: the OpenXR loader only if Setup put it there and it is still ours
        std::string xl = resourceString(IDR_XRLOADER), have;
        const wstring xf = bin + L"\\openxr_loader.dll";
        if (stateGet(g, L"XrLoader") == L"1" && readFile(xf, have) && have == xl) DeleteFileW(xf.c_str());
    }
    DeleteFileW((bin + L"\\fc3vr.log").c_str());
    DeleteFileW((bin + L"\\fc3vr.old.log").c_str());
    DeleteFileW((bin + L"\\fc3vr_bowstring.bin").c_str());  // the mod's learnt bow string, made again on first use

    ui.begin(L"Removing the menu font and controls pictures");
    {
        // only the files Setup put there (and the ones this Setup carries); anything else of yours stays
        const wstring pd = picDir(g);
        vector<wstring> names;
        wstring list = stateGet(g, L"Pictures");
        for (size_t p = 0; p < list.size();) {
            size_t e = list.find(L'|', p);
            if (e == wstring::npos) e = list.size();
            if (e > p) names.push_back(list.substr(p, e - p));
            p = e + 1;
        }
        for (auto& pic : pictures()) names.push_back(pic.name);
        int n = 0;
        for (auto& nm : names)
            if (nm.find_first_of(L"\\/:") == wstring::npos && DeleteFileW((pd + L"\\" + nm).c_str())) n++;
        bool gone = RemoveDirectoryW(pd.c_str()) || !dirExists(pd);
        ui.end(true, n == 0 ? L"none" : gone ? L"removed" : L"removed (your own files kept)");
    }

    ui.begin(L"4 GB memory patch");
    if (o.undoLaa) {
        wstring err;
        int r = largeAddress(gameExe(g), 2, &err);
        ui.end(r == 0, r == 0 ? L"removed" : L"could not remove");
    } else {
        ui.end(true, L"left on");
    }

    ui.begin(L"Steam updates");
    if (o.unlock) {
        wstring m = stateGet(g, L"SteamLock");
        if (m.empty()) m = steamManifest(g);
        if (m.empty() || !fileExists(m)) ui.end(true, L"not a Steam copy");
        else ui.end(setReadOnly(m, false), L"unlocked");
    } else {
        ui.end(true, L"left locked");
    }

    ui.begin(L"Desktop shortcut");
    if (o.removeShortcut) {
        wstring lnk = stateGet(g, L"Shortcut");
        if (lnk.empty()) lnk = desktopShortcut();
        bool gone = false;
        for (int i = 0; i < 10 && !lnk.empty() && fileExists(lnk) && !gone; i++) {  // Explorer may hold it a moment
            gone = DeleteFileW(lnk.c_str());
            if (!gone) Sleep(200);
        }
        ui.end(true, gone ? L"removed" : L"none");
    } else {
        ui.end(true, L"kept");
    }

    ui.begin(L"Removing Blood Dragon VR from Windows' app list");
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, KEY_WOW64_64KEY, 0);
    DeleteFileW(stateFile(g).c_str());
    DeleteFileW((mod + L"\\Setup tips.txt").c_str());
    DeleteFileW((mod + L"\\fc3vr.default.ini").c_str());
    DeleteFileW((mod + L"\\" + kOldGuideName).c_str());
    DeleteFileW((mod + L"\\" + kPdfName).c_str());
    ui.end(true, L"");
    return L"";
}

// After an uninstall run from the game folder, remove our own exe once we've exited.
static void deleteSelfLater(const wstring& g) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    wstring setup = modDir(g) + L"\\" + kSetupName;
    if (lower(fullPath(self)) != lower(fullPath(setup))) {
        DeleteFileW(setup.c_str());
        return;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wstring cmd = L"\"" + wstring(sys) + L"\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + setup + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    MoveFileExW(setup.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // in case the line above can't
}

// ---------------------------------------------------------------- text screen

enum : WORD {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGRAY,
    DGRAY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE
};
static WORD A(WORD fg, WORD bg) { return (WORD)(fg | bg << 4); }

const int W = 80, H = 25;
struct Cell { wchar_t ch; WORD at; };
static Cell scr[H][W];
static HANDLE hOut, hIn, hOrigOut;

static void cls(WORD at) { for (auto& r : scr) for (auto& c : r) c = {L' ', at}; }
static void put(int x, int y, const wstring& s, WORD at) {
    if (y < 0 || y >= H) return;
    for (size_t i = 0; i < s.size(); i++) {
        int xx = x + (int)i;
        if (xx >= 0 && xx < W) scr[y][xx] = {s[i], at};
    }
}
static void putc_(int x, int y, wchar_t c, WORD at) { if (x >= 0 && x < W && y >= 0 && y < H) scr[y][x] = {c, at}; }
static void center(int y, const wstring& s, WORD at) { put((W - (int)s.size()) / 2, y, s, at); }
static void fill(int x, int y, int w, int h, WORD at) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) putc_(i, j, L' ', at);
}
static void shadow(int x, int y, int w, int h) {
    auto dim = [](int i, int j) { if (i >= 0 && i < W && j >= 0 && j < H) scr[j][i].at = A(DGRAY, BLACK); };
    for (int j = y + 1; j <= y + h; j++) { dim(x + w, j); dim(x + w + 1, j); }
    for (int i = x + 2; i < x + w + 2; i++) dim(i, y + h);
}
static void box(int x, int y, int w, int h, WORD at, const wstring& title = L"", bool dbl = true) {
    fill(x, y, w, h, at);
    const wchar_t* c = dbl ? L"╔╗╚╝═║" : L"┌┐└┘─│";
    for (int i = x + 1; i < x + w - 1; i++) { putc_(i, y, c[4], at); putc_(i, y + h - 1, c[4], at); }
    for (int j = y + 1; j < y + h - 1; j++) { putc_(x, j, c[5], at); putc_(x + w - 1, j, c[5], at); }
    putc_(x, y, c[0], at); putc_(x + w - 1, y, c[1], at);
    putc_(x, y + h - 1, c[2], at); putc_(x + w - 1, y + h - 1, c[3], at);
    if (!title.empty()) put(x + (w - (int)title.size() - 2) / 2, y, L" " + title + L" ", at);
    shadow(x, y, w, h);
}
static vector<wstring> wrap(const wstring& s, int width) {
    vector<wstring> out;
    wstring indent;
    size_t lead = s.find_first_not_of(L' ');
    // hang bullets and numbered lines under their text
    if (s.size() > 2 && (s.compare(0, 2, L"* ") == 0 || (iswdigit(s[0]) && s[1] == L'.'))) indent = L"   ";
    wstring line;
    size_t i = 0;
    (void)lead;
    while (i < s.size()) {
        size_t j = s.find(L' ', i);
        if (j == wstring::npos) j = s.size();
        wstring word = s.substr(i, j - i);
        if (!line.empty() && (int)(line.size() + 1 + word.size()) > width) {
            out.push_back(line);
            line = indent + word;
        } else {
            line += (line.empty() ? L"" : L" ") + word;
        }
        i = j + 1;
    }
    out.push_back(line);
    return out;
}

static int g_scrX, g_scrY;   // where our 80x25 screen sits in the console buffer (flush sets it)
static wstring g_hint;       // the bottom bar's text (frame sets it); its "Esc = ..." and "Enter = ..." parts are clickable
static void flush() {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    GetConsoleScreenBufferInfo(hOut, &bi);
    int ww = bi.srWindow.Right - bi.srWindow.Left + 1, wh = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    int ox = std::max(0, (ww - W) / 2), oy = std::max(0, (wh - H) / 2);
    // flicker fix (DrGonzaga 22 Nov: "flashes when I move the mouse"): skip a frame that is the same as the last
    // one, and paint the whole window blue only when the window itself changed, never on every redraw
    static Cell last[H][W];
    static SMALL_RECT lastWin = {-1, -1, -1, -1};
    bool sameWin = !memcmp(&lastWin, &bi.srWindow, sizeof lastWin);
    if (sameWin && !memcmp(last, scr, sizeof last)) return;
    memcpy(last, scr, sizeof last);
    lastWin = bi.srWindow;
    if (!sameWin) {  // so a big window looks like one screen
        DWORD n;
        COORD o = {0, bi.srWindow.Top};
        FillConsoleOutputCharacterW(hOut, L' ', (DWORD)bi.dwSize.X * wh, o, &n);
        FillConsoleOutputAttribute(hOut, A(LGRAY, BLUE), (DWORD)bi.dwSize.X * wh, o, &n);
    }
    CHAR_INFO ci[H * W];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { ci[y * W + x].Char.UnicodeChar = scr[y][x].ch; ci[y * W + x].Attributes = scr[y][x].at; }
    g_scrX = bi.srWindow.Left + ox, g_scrY = bi.srWindow.Top + oy;  // for mouse clicks
    SMALL_RECT r = {(SHORT)(bi.srWindow.Left + ox), (SHORT)(bi.srWindow.Top + oy),
                    (SHORT)(bi.srWindow.Left + ox + W - 1), (SHORT)(bi.srWindow.Top + oy + H - 1)};
    WriteConsoleOutputW(hOut, ci, {W, H}, {0, 0}, &r);
}

// A key press or a mouse action. Mouse: mx, my are cells of our 80x25 screen; move = the pointer reached a new
// cell (hover highlights), click = left button pressed. Right click and the wheel come back as Esc and Up/Down.
struct Key { WORD vk = 0; wchar_t ch = 0; bool resize = false, move = false, click = false; int mx = -1, my = -1; };
static bool hit(const Key& k, int x, int y, int w, int h = 1) {
    return (k.move || k.click) && k.mx >= x && k.mx < x + w && k.my >= y && k.my < y + h;
}
static Key getKey() {
    static int lastX = -1, lastY = -1;
    for (;;) {
        INPUT_RECORD ir;
        DWORD got = 0;
        if (!ReadConsoleInputW(hIn, &ir, 1, &got) || !got) { Sleep(20); continue; }
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) { Key k; k.resize = true; return k; }
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            Key k;
            k.vk = ir.Event.KeyEvent.wVirtualKeyCode;
            k.ch = ir.Event.KeyEvent.uChar.UnicodeChar;
            return k;
        }
        if (ir.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& m = ir.Event.MouseEvent;
            Key k;
            k.mx = m.dwMousePosition.X - g_scrX;
            k.my = m.dwMousePosition.Y - g_scrY;
            if (m.dwEventFlags == MOUSE_WHEELED) { k.vk = (short)HIWORD(m.dwButtonState) > 0 ? VK_UP : VK_DOWN; return k; }
            if (m.dwEventFlags == MOUSE_MOVED) {
                if (k.mx == lastX && k.my == lastY) continue;
                lastX = k.mx, lastY = k.my;
                k.move = true;
                return k;
            }
            if (m.dwEventFlags == 0 || m.dwEventFlags == DOUBLE_CLICK) {
                if (m.dwButtonState & RIGHTMOST_BUTTON_PRESSED) { k.vk = VK_ESCAPE; return k; }
                if (!(m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)) continue;  // a button let go
                if (k.my == H - 1) {  // the bottom bar: "Esc = Back", "Enter = Continue" and so on
                    for (auto [word, vk] : {std::pair<const wchar_t*, WORD>{L"Esc =", VK_ESCAPE}, {L"Enter =", VK_RETURN}}) {
                        size_t at = g_hint.find(word);
                        if (at == wstring::npos) continue;
                        size_t end = g_hint.find(L"   ", at);
                        if (end == wstring::npos) end = g_hint.size();
                        if (k.mx >= 1 + (int)at && k.mx < 1 + (int)end) { Key e; e.vk = vk; return e; }
                    }
                }
                k.click = true;
                return k;
            }
        }
    }
}

static void consoleInit() {
    SetConsoleTitleW(L"Blood Dragon VR Setup");
    hOrigOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    hOut = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) hOut = hOrigOut;
    SetConsoleActiveScreenBuffer(hOut);
    // ask for a classic 80x25 window (Windows Terminal ignores this and we centre instead)
    SMALL_RECT tiny = {0, 0, 0, 0};
    SetConsoleWindowInfo(hOut, TRUE, &tiny);
    SetConsoleScreenBufferSize(hOut, {W, H});
    SMALL_RECT full = {0, 0, W - 1, H - 1};
    SetConsoleWindowInfo(hOut, TRUE, &full);
    CONSOLE_CURSOR_INFO cc = {25, FALSE};
    SetConsoleCursorInfo(hOut, &cc);
    // mouse on; leaving ENABLE_QUICK_EDIT_MODE out stops a click from starting a text selection
    SetConsoleMode(hIn, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
}
static void consoleDone() {
    SetConsoleActiveScreenBuffer(hOrigOut);
    if (hOut != hOrigOut) CloseHandle(hOut);
}

// ---------------------------------------------------------------- screens

static wstring g_game;                 // chosen game folder
static vector<wstring> g_found;        // detected game folders
static const WORD kBg = A(LGRAY, BLUE);
static const WORD kBox = A(WHITE, BLUE);
static const WORD kItem = A(LGRAY, BLUE);
static const WORD kSel = A(BLACK, LCYAN);
static const WORD kDlg = A(BLACK, LGRAY);
static const WORD kErr = A(WHITE, RED);

static const wchar_t* kLogo[] = {
    L" ___ _    ___   ___  ___    ___  ___    _   ___  ___  _  _ ",
    L"| _ ) |  / _ \\ / _ \\|   \\  |   \\| _ \\  /_\\ / __|/ _ \\| \\| |",
    L"| _ \\ |_| (_) | (_) | |) | | |) |   / / _ \\ (_ | (_) | .` |",
    L"|___/____\\___/ \\___/|___/  |___/|_|_\\/_/ \\_\\___|\\___/|_|\\_|",
};
static const wchar_t* kLogoVR[] = {
    L"__   _____ ",
    L"\\ \\ / / _ \\",
    L" \\ V /|   /",
    L"  \\_/ |_|_\\",
};

static wstring shorten(const wstring& s, int n) {
    if ((int)s.size() <= n) return s;
    return L"..." + s.substr(s.size() - (n - 3));
}

static void frame(const wstring& hint) {
    cls(kBg);
    fill(0, 0, W, 1, A(BLACK, LCYAN));
    put(1, 0, L"Blood Dragon VR Setup", A(BLACK, LCYAN));
    wstring b = L"Version " + wstring(kBuild);
    put(W - 1 - (int)b.size(), 0, b, A(BLACK, LCYAN));
    center(0, L"BETA, like its author", A(RED, LCYAN));  // DrGonzaga's tagline (22 Nov)
    int lx = (W - 59 - 12) / 2;
    for (int i = 0; i < 4; i++) {
        put(lx, 2 + i, kLogo[i], A(LMAGENTA, BLUE));
        put(lx + 60, 2 + i, kLogoVR[i], A(LCYAN, BLUE));
    }
    center(6, L"Far Cry 3 Blood Dragon  \x2500  virtual reality mod", A(YELLOW, BLUE));
    fill(0, H - 1, W, 1, A(BLACK, LGRAY));
    put(1, H - 1, hint, A(BLACK, LGRAY));
    g_hint = hint;
}

static void statusLines(int y) {
    wstring game = g_game.empty() ? L"not found yet" : shorten(g_game, 58);
    put(10, y, L"Game     ", A(LCYAN, BLUE));
    put(19, y, game, g_game.empty() ? A(LRED, BLUE) : A(WHITE, BLUE));
    wstring inst = g_game.empty() ? L"" : stateGet(g_game, L"Build");
    if (inst.empty() && !g_game.empty() && (fileExists(binDir(g_game) + L"\\version.dll") || modDllIn(g_game))) inst = L"a VR mod is in the game folder";
    put(10, y + 1, L"Mod      ", A(LCYAN, BLUE));
    put(19, y + 1, inst.empty() ? L"not installed" : (inst.find(L' ') == wstring::npos ? L"installed, build " + inst : inst), A(WHITE, BLUE));
    wstring xr = openXrRuntime();
    put(10, y + 2, L"OpenXR   ", A(LCYAN, BLUE));
    if (xr.empty()) put(19, y + 2, L"no 32-bit runtime found (see Setup tips)", A(YELLOW, BLUE));
    else put(19, y + 2, shorten(fileName(xr), 58) + (lower(xr).find(L"virtualdesktop") != wstring::npos ? L"  (Virtual Desktop)" : L""), A(WHITE, BLUE));
}

// A dialog box in the middle of the screen. Returns the chosen button, or -1 for Esc.
static int dialog(const wstring& title, const vector<wstring>& text, const vector<wstring>& buttons, WORD at,
                  std::function<void()> background, int sel = 0) {
    for (;;) {
        background();
        vector<wstring> lines;
        for (auto& t : text) for (auto& l : wrap(t, 54)) lines.push_back(l);
        int w = 60, h = (int)lines.size() + 6;
        int x = (W - w) / 2, y = std::max(8, (H - h) / 2 + 2);
        if (y + h > H - 2) y = H - 2 - h;
        box(x, y, w, h, at, title);
        for (size_t i = 0; i < lines.size(); i++) put(x + 3, y + 2 + (int)i, lines[i], at);
        int bw = 0;
        for (auto& b : buttons) bw += (int)b.size() + 6;
        int bx = x + (w - bw) / 2;
        vector<std::pair<int, int>> spots;  // each button's x and width, for the mouse
        for (size_t i = 0; i < buttons.size(); i++) {
            wstring s = L" " + buttons[i] + L" ";
            spots.push_back({bx, (int)s.size() + 2});
            WORD ba = (int)i == sel ? A(WHITE, (at >> 4) == RED ? BLACK : RED) : A(BLACK, (at >> 4) == LGRAY ? CYAN : LGRAY);
            put(bx, y + h - 2, L"\x25C4" + s + L"\x25BA", ba);
            bx += (int)s.size() + 6;
        }
        flush();
        Key k = getKey();
        if (k.resize) continue;
        if (k.vk == VK_LEFT || (k.vk == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000))) sel = (sel + (int)buttons.size() - 1) % (int)buttons.size();
        else if (k.vk == VK_RIGHT || k.vk == VK_TAB) sel = (sel + 1) % (int)buttons.size();
        else if (k.vk == VK_RETURN) return sel;
        else if (k.vk == VK_ESCAPE) return -1;
        else if (k.move || k.click) {
            for (size_t i = 0; i < spots.size(); i++)
                if (hit(k, spots[i].first, y + h - 2, spots[i].second)) {
                    sel = (int)i;
                    if (k.click) return (int)i;
                }
        } else for (size_t i = 0; i < buttons.size(); i++)
            if (towlower(k.ch) == towlower(buttons[i][0])) return (int)i;
    }
}

// A list menu. Returns the chosen index, or -1 for Esc.
static int menu(const wstring& title, const vector<wstring>& items, int sel, int y, int w,
                std::function<void()> background) {
    for (;;) {
        background();
        int h = (int)items.size() + 4, x = (W - w) / 2;
        box(x, y, w, h, kBox, title);
        for (size_t i = 0; i < items.size(); i++) {
            bool on = (int)i == sel;
            fill(x + 2, y + 2 + (int)i, w - 4, 1, on ? kSel : kItem);
            put(x + 4, y + 2 + (int)i, items[i], on ? kSel : A(WHITE, BLUE));
            if (on) { putc_(x + 2, y + 2 + (int)i, L'\x25BA', kSel); putc_(x + w - 3, y + 2 + (int)i, L'\x25C4', kSel); }
        }
        flush();
        Key k = getKey();
        if (k.resize) continue;
        int n = (int)items.size();
        if (k.vk == VK_UP) sel = (sel + n - 1) % n;
        else if (k.vk == VK_DOWN) sel = (sel + 1) % n;
        else if (k.vk == VK_HOME) sel = 0;
        else if (k.vk == VK_END) sel = n - 1;
        else if (k.vk == VK_RETURN) return sel;
        else if (k.vk == VK_ESCAPE) return -1;
        else if (k.ch >= L'1' && k.ch < L'1' + n) return k.ch - L'1';
        else for (int i = 0; i < n; i++)
            if (hit(k, x + 2, y + 2 + i, w - 4)) {
                sel = i;
                if (k.click) return i;
            }
    }
}

struct Check { wstring label, help; bool* value; };

// Tick boxes plus a final action line. Returns true to go ahead, false for Esc.
static bool checklist(const wstring& title, vector<Check> items, const wstring& action, std::function<void()> background) {
    int sel = (int)items.size();
    for (;;) {
        background();
        int w = 68, h = (int)items.size() + 9, x = (W - w) / 2, y = 8;
        box(x, y, w, h, kBox, title);
        put(x + 3, y + 1, L"Click or Space ticks a box.  Click the last line (or Enter) to go.", A(LCYAN, BLUE));
        for (size_t i = 0; i < items.size(); i++) {
            bool on = (int)i == sel;
            WORD at = on ? kSel : A(WHITE, BLUE);
            fill(x + 2, y + 3 + (int)i, w - 4, 1, on ? kSel : kItem);
            put(x + 4, y + 3 + (int)i, wstring(L"[") + (*items[i].value ? L"X" : L" ") + L"]  " + items[i].label, at);
        }
        int ay = y + 4 + (int)items.size();
        bool on = sel == (int)items.size();
        fill(x + 2, ay, w - 4, 1, on ? A(WHITE, RED) : kItem);
        put(x + (w - (int)action.size()) / 2, ay, action, on ? A(WHITE, RED) : A(YELLOW, BLUE));
        wstring help = sel < (int)items.size() ? items[sel].help : L"Esc goes back without changing anything.";
        auto hl = wrap(help, w - 8);
        for (size_t i = 0; i < hl.size() && i < 2; i++) put(x + 4, ay + 2 + (int)i, hl[i], A(LGRAY, BLUE));
        flush();
        Key k = getKey();
        if (k.resize) continue;
        int n = (int)items.size() + 1;
        if (k.vk == VK_UP) sel = (sel + n - 1) % n;
        else if (k.vk == VK_DOWN || k.vk == VK_TAB) sel = (sel + 1) % n;
        else if (k.vk == VK_ESCAPE) return false;
        else if ((k.vk == VK_SPACE || k.vk == VK_RETURN) && sel < (int)items.size()) *items[sel].value = !*items[sel].value;
        else if (k.vk == VK_RETURN) return true;
        else {
            for (int i = 0; i < (int)items.size(); i++)
                if (hit(k, x + 2, y + 3 + i, w - 4)) {
                    sel = i;
                    if (k.click) *items[i].value = !*items[i].value;
                }
            if (hit(k, x + 2, ay, w - 4)) {
                sel = (int)items.size();
                if (k.click) return true;
            }
        }
    }
}

// One line of typing. Returns false for Esc.
static bool inputLine(const wstring& title, const wstring& prompt, wstring& val, std::function<void()> background) {
    for (;;) {
        background();
        int w = 72, x = (W - w) / 2, y = 11;
        box(x, y, w, 7, kDlg, title);
        put(x + 3, y + 2, prompt, kDlg);
        fill(x + 3, y + 4, w - 6, 1, A(WHITE, BLACK));
        wstring shown = val.size() > (size_t)(w - 8) ? val.substr(val.size() - (w - 8)) : val;
        put(x + 3, y + 4, shown + L"_", A(WHITE, BLACK));
        put(x + w - 22, y + 5, L"\x25C4 OK \x25BA", A(BLACK, CYAN));
        put(x + w - 13, y + 5, L"\x25C4 Cancel \x25BA", A(BLACK, CYAN));
        flush();
        Key k = getKey();
        if (k.resize) continue;
        if (k.click && hit(k, x + w - 22, y + 5, 6)) return true;
        if (k.click && hit(k, x + w - 13, y + 5, 10)) return false;
        if (k.vk == VK_RETURN) return true;
        if (k.vk == VK_ESCAPE) return false;
        if (k.vk == VK_BACK) { if (!val.empty()) val.pop_back(); }
        else if (k.ch >= 32) val += k.ch;
    }
}

static void viewTips() {
    vector<std::pair<wstring, WORD>> lines;
    wstring all = utf8(resourceString(IDR_TIPS));
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find(L'\n', p);
        if (e == wstring::npos) e = all.size();
        wstring l = all.substr(p, e - p);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (!l.empty() && l[0] == L'#') {
            if (!lines.empty()) lines.push_back({L"", kItem});
            wstring t = l.substr(l.find_first_not_of(L"# "));
            lines.push_back({t, A(YELLOW, BLUE)});
            lines.push_back({wstring(t.size(), L'\x2500'), A(BROWN, BLUE)});
        } else if (!l.empty()) {
            for (auto& w : wrap(l, 70)) lines.push_back({w, A(WHITE, BLUE)});
        }
        p = e + 1;
    }
    int top = 0;
    for (;;) {
        frame(L"\x2191\x2193 PgUp PgDn = Scroll   Esc = Back");
        int x = 2, y = 8, w = W - 4, h = H - 9;
        box(x, y, w, h, kBox, L"Setup tips");
        put(x + w - 12, y + h - 1, L"\x25C4 Back \x25BA", A(BLACK, CYAN));
        int rows = h - 2;
        int maxTop = std::max(0, (int)lines.size() - rows);
        top = std::min(std::max(top, 0), maxTop);
        for (int i = 0; i < rows && top + i < (int)lines.size(); i++)
            put(x + 3, y + 1 + i, lines[top + i].first, lines[top + i].second);
        if (maxTop > 0) {  // scroll bar
            for (int i = 0; i < rows; i++) putc_(x + w - 1, y + 1 + i, L'\x2591', A(LCYAN, BLUE));
            int t = rows > 1 ? top * (rows - 1) / maxTop : 0;
            putc_(x + w - 1, y + 1 + t, L'\x2588', A(LCYAN, BLUE));
        }
        flush();
        Key k = getKey();
        if (k.resize) continue;
        if (k.vk == VK_UP) top--;
        else if (k.vk == VK_DOWN) top++;
        else if (k.vk == VK_PRIOR) top -= rows - 1;
        else if (k.vk == VK_NEXT || k.vk == VK_SPACE) top += rows - 1;
        else if (k.vk == VK_HOME) top = 0;
        else if (k.vk == VK_END) top = maxTop;
        else if (k.vk == VK_ESCAPE || k.vk == VK_RETURN) return;
        else if (k.click && hit(k, x + w - 12, y + h - 1, 8)) return;
        else if (k.click && hit(k, x + w - 1, y + 1, 1, rows) && maxTop > 0)  // scroll bar: jump there
            top = (k.my - y - 1) * maxTop / std::max(1, rows - 1);
    }
}

// Opens the controls PDF (all the controls pictures, one per page): the installed copy, or a fresh one in Temp.
static void openPictures() {
    wstring p = g_game.empty() ? L"" : modDir(g_game) + L"\\" + kPdfName;
    if (p.empty() || !fileExists(p)) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        p = wstring(tmp) + kPdfName;
        std::string pdf = resourceString(IDR_CONTROLSPDF);
        writeFile(p, pdf.data(), pdf.size(), nullptr);
    }
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void mainBackground() {
    frame(L"Click, or \x2191\x2193 and Enter   Esc = Exit");
    statusLines(8);
}

static void tickBackground() { frame(L"Click, or \x2191\x2193 Space Enter   Esc = Back"); }

// Windows' own folder picker. Returns an empty string if cancelled.
static wstring browseFolder() {
    wstring out;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD f = 0;
        d->GetOptions(&f);
        d->SetOptions(f | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Pick the Far Cry 3 Blood Dragon game folder");
        if (SUCCEEDED(d->Show(GetConsoleWindow()))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path, CoTaskMemFree(path);
                it->Release();
            }
        }
        d->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return out;
}

// Lets the person confirm or change the game folder. Returns false to go back.
static bool chooseGame() {
    for (;;) {
        vector<wstring> items;
        for (auto& f : g_found) {
            wstring tag = steamOwns(f) ? L"  [Steam's copy]" : L"  [old copy?]";
            if (g_found.size() == 1) tag = L"";
            items.push_back(shorten(f, 62 - (int)tag.size()) + tag);
        }
        items.push_back(L"Browse for the game folder...");
        items.push_back(L"Type the game folder myself...");
        int sel = 0;
        for (size_t i = 0; i < g_found.size(); i++) if (lower(g_found[i]) == lower(g_game)) sel = (int)i;
        auto bg = [] { frame(L"Click, or \x2191\x2193 and Enter   Esc = Back"); };
        int c = menu(g_found.empty() ? L"Blood Dragon was not found: where is it?" : L"Which game folder?", items, sel, 9, 70, bg);
        if (c < 0) return false;
        if (c < (int)g_found.size()) { g_game = g_found[c]; return true; }
        wstring typed = g_game;
        if (c == (int)g_found.size()) {
            typed = browseFolder();
            if (typed.empty()) continue;
        } else if (!inputLine(L"Game folder", L"The folder that holds bin\\fc3_blooddragon_d3d11.exe:", typed, bg)) continue;
        wstring g = normaliseGameDir(typed);
        if (isGameDir(g)) {
            g_game = g;
            if (std::find_if(g_found.begin(), g_found.end(), [&](const wstring& f) { return lower(f) == lower(g); }) == g_found.end())
                g_found.push_back(g);
            return true;
        }
        dialog(L"Not found", {L"There is no bin\\fc3_blooddragon_d3d11.exe in:", shorten(g, 54),
                              L"In Steam you can find it with: right-click the game, Manage, Browse local files."},
               {L"OK"}, kErr, bg);
    }
}

// Runs install or uninstall steps with a DOS-style progress screen.
static wstring runSteps(const wstring& title, int total, std::function<wstring(const Step&)> work) {
    vector<std::pair<wstring, wstring>> done;  // label, result
    wstring current;
    auto draw = [&]() {
        frame(L"Working...  please wait");
        int x = 8, y = 8, w = 64, h = 15;
        box(x, y, w, h, kBox, title);
        int row = y + 2;
        for (auto& d : done) {
            put(x + 3, row, d.first, A(LGRAY, BLUE));
            int dots = x + 3 + (int)d.first.size() + 1;
            bool ok = d.second.compare(0, 4, L"FAIL") != 0;
            wstring res = ok ? (d.second.empty() ? L"OK" : d.second) : d.second;
            int rx = x + w - 4 - (int)res.size();
            for (int i = dots; i < rx - 1; i++) putc_(i, row, L'.', A(DGRAY, BLUE));
            put(rx, row, res, ok ? A(LGREEN, BLUE) : A(LRED, BLUE));
            row++;
        }
        if (!current.empty()) { put(x + 3, row, current, A(WHITE, BLUE)); put(x + 4 + (int)current.size(), row, L"\x2588", A(YELLOW, BLUE)); }
        int bar = w - 12, n = total ? (int)done.size() * bar / total : bar;
        put(x + 3, y + h - 3, L"[", A(WHITE, BLUE));
        for (int i = 0; i < bar; i++) putc_(x + 4 + i, y + h - 3, i < n ? L'\x2593' : L'\x2591', i < n ? A(YELLOW, BLUE) : A(DGRAY, BLUE));
        put(x + 4 + bar, y + h - 3, L"]", A(WHITE, BLUE));
        wchar_t pct[8];
        swprintf(pct, 8, L"%3d%%", total ? (int)done.size() * 100 / total : 100);
        put(x + 6 + bar, y + h - 3, pct, A(WHITE, BLUE));
        flush();
    };
    Step ui;
    ui.begin = [&](const wstring& s) { current = s; draw(); Sleep(220); };
    ui.end = [&](bool ok, const wstring& note) {
        done.push_back({current, ok ? note : L"FAIL" + (note.empty() ? L"" : L" " + note)});
        current.clear();
        draw();
        Sleep(120);
    };
    wstring err = work(ui);
    draw();
    return err;
}

static void installFlow() {
    if (!chooseGame()) return;
    if (!knownGameVersion(g_game)) {
        int c = dialog(L"Different game version", {
            L"Blood Dragon VR was made for the Steam copy of the game after its last update. This copy's "
            L"FC3_d3d11.dll is different, so it may be another version (Ubisoft Connect, an older patch or a "
            L"changed file), and the mod may not work with it.",
            L"",
            L"Steam owners: Verify integrity of game files in Steam, then run Setup again."},
            {L"Install anyway", L"Back"}, kErr, [] { frame(L"\x2190\x2192 = Choose   Enter = OK"); }, 1);
        if (c != 0) return;
    }
    InstallOptions o;
    const bool compat = compatFixFound(g_game);
    o.dinput8 = compat || currentLoader(g_game) == 1;
    bool hasIni = fileExists(binDir(g_game) + L"\\fc3vr.ini") || fileExists(binDir(g_game) + L"\\" + kPromptsName);
    bool reinstall = fileExists(stateFile(g_game));
    vector<Check> c = {
        {L"4 GB memory patch for the game (recommended)",
         L"Lets the game use 4 GB of memory instead of 2 GB, which VR needs. Setup can undo it.", &o.laa},
        {L"Stop Steam updating the game (recommended)",
         L"Locks Steam's update file for Blood Dragon so an update can't undo the mod. Setup can undo it.", &o.steamLock},
        {L"Desktop shortcut that starts the game in VR",
         L"Starts the game's own program directly, skipping the Steam and Ubisoft launch screens.", &o.shortcut},
    };
    c.push_back({compat ? L"Load the mod as dinput8.dll (needed: Windows CPU fix found)" : L"Load the mod as dinput8.dll (only if VR didn't start)",
                 L"For players who fixed the game's start-up crash on CPUs with many threads with a Windows compatibility fix: "
                 L"that fix stops version.dll loading. See Setup tips.", &o.dinput8});
    if (hasIni)
        c.push_back({L"Keep my current settings (fc3vr.ini, bdvr-prompts.txt)",
                     L"Untick to start again from the default settings. Your current file is backed up either way.", &o.keepIni});
    if (!checklist(reinstall ? L"Update Blood Dragon VR" : L"Install Blood Dragon VR", c,
                   reinstall ? L"\x25BA  UPDATE NOW  \x25C4" : L"\x25BA  INSTALL NOW  \x25C4", tickBackground))
        return;
    wstring err = runSteps(L"Installing", 11, [&](const Step& ui) { return doInstall(g_game, o, ui); });
    auto done = [] { frame(L"Enter = Continue"); };
    if (!err.empty()) {
        dialog(L"Install stopped", {err, L"Nothing else was changed after this point. You can run Setup again."}, {L"OK"}, kErr, done);
        return;
    }
    int c2 = dialog(L"Install complete", {
        L"Blood Dragon VR " + wstring(kBuild) + L" is installed.",
        L"",
        L"1. NVIDIA cards: set Threaded optimization to Off in NVIDIA Control Panel (see Setup tips). AMD: nothing to set.",
        L"2. Connect the headset to the PC in Virtual Desktop.",
        L"3. Start the game from the Blood Dragon VR shortcut on your desktop."},
        {L"Controls guide", L"Setup tips", L"Main menu"}, kDlg, done);
    if (c2 == 0) openPictures();
    else if (c2 == 1) viewTips();
}

static void uninstallFlow(bool fromApps) {
    if (!fromApps && !chooseGame()) return;
    if (!fileExists(stateFile(g_game)) && !fileExists(binDir(g_game) + L"\\version.dll") && !modDllIn(g_game)) {
        dialog(L"Nothing to remove", {L"Blood Dragon VR is not installed in:", shorten(g_game, 54)}, {L"OK"}, kDlg, mainBackground);
        return;
    }
    UninstallOptions o;
    o.undoLaa = stateGet(g_game, L"Laa") == L"1";
    o.unlock = !stateGet(g_game, L"SteamLock").empty();
    vector<Check> c = {
        {L"Keep my settings (fc3vr.ini, bdvr-prompts.txt) for next time",
         L"Untick to move it to the BloodDragonVR\\backup folder.", &o.keepIni},
        {L"Undo the 4 GB memory patch",
         L"Ticked if Setup applied it. Leaving it on does no harm to the normal game.", &o.undoLaa},
        {L"Let Steam update the game again",
         L"Unlocks Steam's update file for the game.", &o.unlock},
        {L"Remove the desktop shortcut", L"", &o.removeShortcut},
    };
    if (!checklist(L"Remove Blood Dragon VR", c, L"\x25BA  REMOVE NOW  \x25C4", tickBackground)) return;
    wstring err = runSteps(L"Removing", 8, [&](const Step& ui) { return doUninstall(g_game, o, ui); });
    auto done = [] { frame(L"Enter = Continue"); };
    if (!err.empty()) {
        dialog(L"Remove stopped", {err}, {L"OK"}, kErr, done);
        return;
    }
    dialog(L"Removed", {L"Blood Dragon VR has been removed. The game is back to normal.",
                        L"Your backups stay in the BloodDragonVR\\backup folder inside the game folder."},
           {L"OK"}, kDlg, done);
    deleteSelfLater(g_game);
}

static int interactive(bool uninstallOnly) {
    consoleInit();
    g_found = findGames();
    // started from the game's own BloodDragonVR folder: that copy of the game comes first
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    wstring here = parentDir(parentDir(fullPath(self)));
    if (lower(fileName(parentDir(fullPath(self)))) == lower(kModFolder) && isGameDir(here)) {
        g_game = here;
        if (std::find_if(g_found.begin(), g_found.end(), [&](const wstring& f) { return lower(f) == lower(here); }) == g_found.end())
            g_found.insert(g_found.begin(), here);
    } else if (!g_found.empty()) {
        g_game = g_found[0];
    }
    if (uninstallOnly) {
        if (g_game.empty() && !chooseGame()) { consoleDone(); return 1; }
        uninstallFlow(true);
        consoleDone();
        return 0;
    }
    int sel = 0;
    for (;;) {
        bool installed = !g_game.empty() && fileExists(stateFile(g_game));
        vector<wstring> items = {installed ? L"1  Update Blood Dragon VR" : L"1  Install Blood Dragon VR",
                                 L"2  Uninstall", L"3  Controls guide  (opens the PDF)",
                                 L"4  Setup tips  (NVIDIA, Ubisoft, starting up)", L"5  Exit"};
        int c = menu(L"Main menu", items, sel, 12, 54, mainBackground);
        if (c < 0 || c == 4) break;
        sel = c;
        if (c == 0) installFlow();
        else if (c == 1) uninstallFlow(false);
        else if (c == 2) openPictures();
        else if (c == 3) viewTips();
    }
    consoleDone();
    return 0;
}

// ---------------------------------------------------------------- scripted runs (for testing)

static int scripted(bool install, const wstring& dir, const vector<wstring>& flags) {
    auto has = [&](const wchar_t* f) {
        for (auto& x : flags) if (lower(x) == f) return true;
        return false;
    };
    wstring g = normaliseGameDir(dir);
    if (!isGameDir(g)) { wprintf(L"error: no %ls under %ls\n", kExeName, g.c_str()); return 2; }
    wstring cur;
    Step ui;
    ui.begin = [&](const wstring& s) { cur = s; };
    ui.end = [&](bool ok, const wstring& n) { wprintf(L"%-48ls %ls %ls\n", cur.c_str(), ok ? L"OK" : L"FAIL", n.c_str()); };
    wstring err;
    if (install) {
        InstallOptions o;
        o.laa = !has(L"/nolaa");
        o.steamLock = !has(L"/nolock");
        o.shortcut = !has(L"/noshortcut");
        o.keepIni = !has(L"/resetini");
        o.dinput8 = has(L"/dinput8") || (!has(L"/version") && (compatFixFound(g) || currentLoader(g) == 1));
        err = doInstall(g, o, ui);
    } else {
        UninstallOptions o;
        o.keepIni = !has(L"/removeini");
        o.undoLaa = !has(L"/keeplaa") && stateGet(g, L"Laa") == L"1";
        o.unlock = !has(L"/keeplock");
        err = doUninstall(g, o, ui);
    }
    if (!err.empty()) { wprintf(L"error: %ls\n", err.c_str()); return 1; }
    wprintf(L"done\n");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    vector<wstring> args(argv + 1, argv + argc);
    vector<wstring> flags;
    wstring mode, dir;
    for (auto& a : args) {
        wstring l = lower(a);
        if (l == L"/install" || l == L"/uninstall" || l == L"/find") mode = l;
        else if (!a.empty() && a[0] == L'/') flags.push_back(a);
        else dir = a;
    }
    if (mode == L"/find") {
        for (auto& f : findGames()) wprintf(L"%ls\n", f.c_str());
        wprintf(L"openxr32: %ls\n", openXrRuntime().c_str());
        return 0;
    }
    if (mode == L"/install" && !dir.empty()) return scripted(true, dir, flags);
    if (mode == L"/uninstall" && !dir.empty()) return scripted(false, dir, flags);
    return interactive(mode == L"/uninstall");
}

#pragma once

#include <cstdint>
#include <string>
#include <mutex>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #define DYNLIB HMODULE
    #define LOADLIB(path) ::LoadLibraryW(path)
    #define GETSYM ::GetProcAddress
    #define CLOSELIB ::FreeLibrary
    #define STEAM_CALL __cdecl
#elif defined(__APPLE__) || defined(__linux__)
    #include <dlfcn.h>
    #define DYNLIB void*
    #define LOADLIB(path) dlopen(path, RTLD_LAZY | RTLD_LOCAL)
    #define GETSYM dlsym
    #define CLOSELIB dlclose
    #define STEAM_CALL
#else
    #error Unsupported platform
#endif

static constexpr char kOskMsg[] = "steam_osk";

enum EFloatingGamepadTextInputMode {
    k_EFloatingGamepadTextInputModeModeSingleLine = 0,
    k_EFloatingGamepadTextInputModeModeMultipleLines = 1,
    k_EFloatingGamepadTextInputModeModeEmail = 2,
    k_EFloatingGamepadTextInputModeModeNumeric = 3
};

enum SteamAPIInitResult {
    SteamAPIInitResult_OK = 0,
    SteamAPIInitResult_FailedGeneric = 1,
    SteamAPIInitResult_NoSteamClient = 2,
    SteamAPIInitResult_VersionMismatch = 3
};

struct ISteamUtils;

class Steam {
public:
    static Steam& Instance() { 
        static Steam s; 
        return s; 
    }

    bool ShouldShowOsk() {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!ensureLoadedAndInit()) {
            LOG(INFO) << "Steam ShouldShowOsk is false because ensureLoadedAndInit is false.";
            return false;
        }

        ISteamUtils* utils = getUtils();
        if (!utils) {
            LOG(INFO) << "Steam ShouldShowOsk is false because getUtils is null.";
            return false;
        }

        const bool overlayOK = p_SteamAPI_ISteamUtils_IsOverlayEnabled ? p_SteamAPI_ISteamUtils_IsOverlayEnabled(utils) : true;
        const bool inBP = p_SteamAPI_ISteamUtils_IsSteamInBigPictureMode ? p_SteamAPI_ISteamUtils_IsSteamInBigPictureMode(utils) : false;
        const bool onDeck = p_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck ? p_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(utils) : false;
        const bool result = overlayOK && (inBP || onDeck);
        LOG(INFO) << "Steam ShouldShowOsk (overlayOK = " << overlayOK << ", inBP = " << inBP << ", onDeck = " << onDeck << ") = " << result;
        return overlayOK && (inBP || onDeck);
    }

    bool ShowOsk(int x, int y, int w, int h, EFloatingGamepadTextInputMode mode) {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!ensureLoadedAndInit()) return false;
        ISteamUtils* utils = getUtils();
        if (!utils || !p_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput) return false;
        _shown = p_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput(utils, (int)mode, x, y, w, h);
        return _shown;
    }

    void DismissOsk() {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!_shown) return;
        if (auto* utils = getUtils()) {
            if (p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput)
                p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput(utils);
        }
        _shown = false;
    }

private:
    Steam() = default;
    ~Steam() {
        if (p_SteamAPI_Shutdown && _didInit) p_SteamAPI_Shutdown();
        unload();
    }
    Steam(const Steam&) = delete;
    Steam& operator=(const Steam&) = delete;

    using fnSteamAPI_Init = bool (STEAM_CALL*)();
    using fnSteamAPI_InitSafe = bool (STEAM_CALL*)();
    using fnSteamAPI_InitFlat = SteamAPIInitResult (STEAM_CALL*)(char* /*outErr*/);
    using fnSteamAPI_Shutdown = void (STEAM_CALL*)();
    using fnSteamAPI_IsSteamRunning = bool (STEAM_CALL*)();
    using fnSteamAPI_SteamUtils_ver = ISteamUtils* (STEAM_CALL*)();
    using fnSteamAPI_ISteamUtils_IsOverlayEnabled = bool (STEAM_CALL*)(ISteamUtils*);
    using fnSteamAPI_ISteamUtils_IsSteamInBigPictureMode = bool (STEAM_CALL*)(ISteamUtils*);
    using fnSteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = bool (STEAM_CALL*)(ISteamUtils*);
    using fnSteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = bool (STEAM_CALL*)(ISteamUtils*, int /*mode*/, int,int,int,int);
    using fnSteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = bool (STEAM_CALL*)(ISteamUtils*);

    template<class T> void resolve(T& fp, const char* name) {
#if defined(_WIN32)
        fp = reinterpret_cast<T>(GETSYM(_dll, name));
#else
        fp = reinterpret_cast<T>(GETSYM(_dll, name));
#endif
    }

#define RESOLVE(var, name) resolve(var, name)

    bool ensureLoadedAndInit() {
        if (_didInit) return true;

        LOG(INFO) << "Steam tryInit.";
        if (!_dll) {
            if (!loadAnySteamApi()) {
                LOG(INFO) << "Steam failed to init because loadAnySteamApi returned false.";
                return false;
            }
        }

        RESOLVE(p_SteamAPI_InitFlat, "SteamAPI_InitFlat");
        RESOLVE(p_SteamAPI_InitSafe, "SteamAPI_InitSafe");
        RESOLVE(p_SteamAPI_Init, "SteamAPI_Init");
        RESOLVE(p_SteamAPI_Shutdown, "SteamAPI_Shutdown");
        RESOLVE(p_SteamAPI_IsSteamRunning, "SteamAPI_IsSteamRunning");

        if (!resolveUtilsGetter()) {
            LOG(INFO) << "Steam failed to init because resolveUtilsGetter returned false.";
            return false;
        }

        RESOLVE(p_SteamAPI_ISteamUtils_IsOverlayEnabled, "SteamAPI_ISteamUtils_IsOverlayEnabled");
        RESOLVE(p_SteamAPI_ISteamUtils_IsSteamInBigPictureMode, "SteamAPI_ISteamUtils_IsSteamInBigPictureMode");
        RESOLVE(p_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck, "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");
        RESOLVE(p_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput, "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput");
        RESOLVE(p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput, "SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput");
        if (!p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput) {
            RESOLVE(p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput, "SteamAPI_ISteamUtils_DismissGamepadTextInput");
        }

        if (!p_SteamAPI_IsSteamRunning) {
            LOG(INFO) << "Steam warning: p_SteamAPI_IsSteamRunning is null.";
        } else if (!p_SteamAPI_IsSteamRunning()) {
            LOG(INFO) << "Steam failed to init because Steam is not running.";
            return false;
        }

        if (p_SteamAPI_InitFlat) {
            char err[1024] = {};
            SteamAPIInitResult r = p_SteamAPI_InitFlat(err);
            _didInit = (r == SteamAPIInitResult_OK);
            LOG(INFO) << "Steam initialized using SteamAPI_InitFlat: " << _didInit << ( _didInit ? "" : std::string(" (reason: ") + err + ")" );
        } else if (p_SteamAPI_InitSafe) {
            _didInit = p_SteamAPI_InitSafe();
            LOG(INFO) << "Steam initialized using SteamAPI_InitSafe: " << _didInit;
        } else if (p_SteamAPI_Init) {
            _didInit = p_SteamAPI_Init();
            LOG(INFO) << "Steam initialized using SteamAPI_Init: " << _didInit;
        } else {
            LOG(INFO) << "Steam failed to init because no suitable Init symbol was found.";
            return false;
        }

        if (!_didInit) {
            LOG(INFO) << "Steam failed to init because Init returned false.";
            return false;
        }

        LOG(INFO) << "Steam successfully initialized.";
        return _didInit;
    }

    bool loadAnySteamApi() {
#if defined(_WIN64)
        const wchar_t* candsW[] = { L"steam_api64.dll", L"steam_api.dll" };
        for (auto* n : candsW) { _dll = LOADLIB(n); if (_dll) return true; }
#elif defined(_WIN32)
        const wchar_t* candsW[] = { L"steam_api.dll" };
        for (auto* n : candsW) { _dll = LOADLIB(n); if (_dll) return true; }
#elif defined(__APPLE__)
        const char* cands[] = { "libsteam_api.dylib" };
        for (auto* n : cands) { _dll = LOADLIB(n); if (_dll) return true; }
#elif defined(__linux__)
        const char* cands[] = { "libsteam_api.so" };
        for (auto* n : cands) { _dll = LOADLIB(n); if (_dll) return true; }
#endif
        return false;
    }

    void unload() {
        if (_dll) { CLOSELIB(_dll); _dll = nullptr; }
    }

    bool resolveUtilsGetter() {
        const char* getters[] = {
            "SteamAPI_SteamUtils_v014", "SteamAPI_SteamUtils_v013", "SteamAPI_SteamUtils_v012",
            "SteamAPI_SteamUtils_v011", "SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009"
        };
        for (auto* g : getters) {
            auto* sym = (fnSteamAPI_SteamUtils_ver)GETSYM(_dll, g);
            if (sym) { p_SteamAPI_SteamUtils_any = sym; return true; }
        }
        return false;
    }

    ISteamUtils* getUtils() {
        return p_SteamAPI_SteamUtils_any ? p_SteamAPI_SteamUtils_any() : nullptr;
    }

    std::mutex _mutex;
    DYNLIB _dll = nullptr;
    bool _didInit = false;
    bool _shown = false;

    fnSteamAPI_Init p_SteamAPI_Init = nullptr;
    fnSteamAPI_InitSafe p_SteamAPI_InitSafe = nullptr;
    fnSteamAPI_InitFlat p_SteamAPI_InitFlat = nullptr;
    fnSteamAPI_Shutdown p_SteamAPI_Shutdown = nullptr;
    fnSteamAPI_IsSteamRunning p_SteamAPI_IsSteamRunning = nullptr;
    fnSteamAPI_SteamUtils_ver p_SteamAPI_SteamUtils_any = nullptr;
    fnSteamAPI_ISteamUtils_IsOverlayEnabled p_SteamAPI_ISteamUtils_IsOverlayEnabled = nullptr;
    fnSteamAPI_ISteamUtils_IsSteamInBigPictureMode p_SteamAPI_ISteamUtils_IsSteamInBigPictureMode = nullptr;
    fnSteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck p_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = nullptr;
    fnSteamAPI_ISteamUtils_ShowFloatingGamepadTextInput p_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = nullptr;
    fnSteamAPI_ISteamUtils_DismissFloatingGamepadTextInput p_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = nullptr;
};

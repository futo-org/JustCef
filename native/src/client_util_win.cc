#include "client_util.h"
#include "client.h"

#include <windows.h>
#include <string>
#include <commdlg.h>
#include <shlobj.h>
#include <optional>

#include "include/cef_browser.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

std::map<int, WINDOWPLACEMENT> _savedPlacements;

std::wstring StringToWString(const std::string& str) 
{
    if (str.empty()) 
        return std::wstring();

    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (count == 0) {
        return std::wstring();
    }

    std::wstring wstr(count - 1, L'\0');  // Allocate space for wide string
    int ret = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], count);
    if (ret == 0) {
        return std::wstring();
    }

    return wstr;
}

std::string WStringToString(const std::wstring& wstr)
{
    if (wstr.empty())
        return std::string();

    int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (count == 0) {
        return std::string();
    }

    std::string str(count - 1, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
    if (ret == 0) {
        return std::string();
    }

    return str;
}

namespace shared {
    void PlatformTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title) 
    {
        HWND hwnd = browser->GetHost()->GetWindowHandle();
        SetWindowText(hwnd, StringToWString(title).c_str());
    }

    void PlatformIconChange(CefRefPtr<CefBrowser> browser, const std::string& iconPath) 
    {
        int width, height, channels;
        unsigned char* image = stbi_load(iconPath.c_str(), &width, &height, &channels, 4);
        if (!image) {
            return;
        }

        for (int i = 0; i < width * height; ++i) 
        {
            unsigned char temp = image[i * 4];
            image[i * 4] = image[i * 4 + 2];
            image[i * 4 + 2] = temp;
        }

        HICON hIcon = NULL;
        {
            HBITMAP hBitmap = CreateBitmap(width, height, 1, 32, image);
            if (hBitmap) {
                ICONINFO iconInfo = {0};
                iconInfo.fIcon = TRUE;
                iconInfo.hbmMask = hBitmap;
                iconInfo.hbmColor = hBitmap;
                hIcon = CreateIconIndirect(&iconInfo);
                DeleteObject(hBitmap);
            }
        }

        if (hIcon) {
            HWND hwnd = browser->GetHost()->GetWindowHandle();
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            //TODO: Should hIcon be freed?
        }

        stbi_image_free(image);
    }

    bool PlatformGetFullscreen(CefRefPtr<CefBrowser> browser)
    {
        CefWindowHandle hwnd = browser->GetHost()->GetWindowHandle();
        WINDOWPLACEMENT wp = {};
        GetWindowPlacement(hwnd, &wp);
        return (wp.showCmd == SW_SHOWMAXIMIZED);
    }

    void PlatformSetMinimumWindowSize(CefRefPtr<CefBrowser> browser, int minWidth, int minHeight)
    {
        /* No implementation required, implemented through WndProc hook */
        CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
        Client* pClient = (Client*)client.get();
        if (pClient) {
            pClient->settings.minimumWidth = minWidth;
            pClient->settings.minimumHeight = minHeight;
        }
    }

    void PlatformSetFrameless(CefRefPtr<CefBrowser> browser, bool frameless)
    {
        LOG(INFO) << "PlatformSetFrameless: " << frameless;
        CefWindowHandle hwnd = browser->GetHost()->GetWindowHandle();
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if (frameless) {
            style &= ~(WS_CAPTION | WS_THICKFRAME);
        } else {
            style |= (WS_CAPTION | WS_THICKFRAME);
        }
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    void PlatformSetResizable(CefRefPtr<CefBrowser> browser, bool resizable)
    {
        LOG(INFO) << "PlatformSetResizable: " << resizable;
        CefWindowHandle hwnd = browser->GetHost()->GetWindowHandle();
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if (resizable) {
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        } else {
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    void PlatformSetFullscreen(CefRefPtr<CefBrowser> browser, bool fullscreen)
    {
        LOG(INFO) << "PlatformSetFullscreen: " << fullscreen;

        CefWindowHandle hwnd = browser->GetHost()->GetWindowHandle();
        if (fullscreen) {
            WINDOWPLACEMENT placement = {};
            if (GetWindowPlacement(hwnd, &placement))
                _savedPlacements[browser->GetIdentifier()] = placement;
            SetWindowLongPtr(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);

            HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
            if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                SetWindowPos(hwnd, HWND_TOP,
                    monitorInfo.rcMonitor.left,
                    monitorInfo.rcMonitor.top,
                    monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                    monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                    SWP_FRAMECHANGED);
            } else {
                SetWindowPos(hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
            }
        } else {
            SetWindowLongPtr(hwnd, GWL_STYLE, WS_VISIBLE | WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);

            auto itr = _savedPlacements.find(browser->GetIdentifier());
            if (itr != _savedPlacements.end()) {
                auto placement = itr->second;
                SetWindowPlacement(hwnd, &placement);
                _savedPlacements.erase(browser->GetIdentifier());
            } else {
                ShowWindow(hwnd, SW_SHOWNORMAL);
            }
        }
    }

    void PlatformMaximize(CefRefPtr<CefBrowser> browser)
    {
        ShowWindow(browser->GetHost()->GetWindowHandle(), SW_MAXIMIZE);
    }

    void PlatformMinimize(CefRefPtr<CefBrowser> browser)
    {
        ShowWindow(browser->GetHost()->GetWindowHandle(), SW_MINIMIZE);
    }

    void PlatformRestore(CefRefPtr<CefBrowser> browser)
    {
        ShowWindow(browser->GetHost()->GetWindowHandle(), SW_RESTORE);
    }

    void PlatformShow(CefRefPtr<CefBrowser> browser)
    {
        ShowWindow(browser->GetHost()->GetWindowHandle(), SW_SHOW);
    }

    void PlatformHide(CefRefPtr<CefBrowser> browser)
    {
        ShowWindow(browser->GetHost()->GetWindowHandle(), SW_HIDE);
    }

    void PlatformActivate(CefRefPtr<CefBrowser> browser)
    {
        SetForegroundWindow(browser->GetHost()->GetWindowHandle());
    }

    void PlatformBringToTop(CefRefPtr<CefBrowser> browser)
    {
        BringWindowToTop(browser->GetHost()->GetWindowHandle());
    }

    void PlatformSetAlwaysOnTop(CefRefPtr<CefBrowser> browser, bool alwaysOnTop)
    {
        SetWindowPos(browser->GetHost()->GetWindowHandle(), alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    CefSize PlatformGetWindowSize(CefRefPtr<CefBrowser> browser)
    {
        RECT rect;
        GetWindowRect(browser->GetHost()->GetWindowHandle(), &rect);
        return CefSize(rect.right - rect.left, rect.bottom - rect.top);
    }

    void PlatformCenterWindow(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenWidth - size.width) / 2;
        int y = (screenHeight - size.height) / 2;
        SetWindowPos(browser->GetHost()->GetWindowHandle(), NULL, x, y, size.width, size.height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void PlatformSetWindowSize(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        HWND hwnd = browser->GetHost()->GetWindowHandle();
        SetWindowPos(hwnd, NULL, 0, 0, size.width, size.height, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }

    CefPoint PlatformGetWindowPosition(CefRefPtr<CefBrowser> browser)
    {
        RECT rect;
        GetWindowRect(browser->GetHost()->GetWindowHandle(), &rect);
        return CefPoint(rect.left, rect.top);
    }

    void PlatformSetWindowPosition(CefRefPtr<CefBrowser> browser, const CefPoint& position)
    {
        SetWindowPos(browser->GetHost()->GetWindowHandle(), NULL, position.x, position.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void PlatformWindowRequestFocus(CefRefPtr<CefBrowser> browser)
    {
        SetFocus(browser->GetHost()->GetWindowHandle());
    }

    std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string, std::string>>& filters)
    {
        std::promise<std::vector<std::string>> promise;
        HRESULT hr;

        IFileOpenDialog* pFileOpen = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
        if (SUCCEEDED(hr)) {
            DWORD dwOptions;
            hr = pFileOpen->GetOptions(&dwOptions);
            if (SUCCEEDED(hr)) {
                if (multiple) {
                    hr = pFileOpen->SetOptions(dwOptions | FOS_ALLOWMULTISELECT);
                }
            }

            std::vector<std::wstring> filterNames;
            std::vector<std::wstring> filterSpecs;
            filterNames.reserve(filters.size());
            filterSpecs.reserve(filters.size());

            for (const auto& filter : filters) {
                filterNames.push_back(StringToWString(filter.first));
                filterSpecs.push_back(StringToWString(filter.second));
            }

            std::vector<COMDLG_FILTERSPEC> fileTypes;
            fileTypes.reserve(filters.size());
            for (size_t i = 0; i < filters.size(); ++i) {
                fileTypes.push_back({ filterNames[i].c_str(), filterSpecs[i].c_str() });
            }

            if (!fileTypes.empty()) {
                pFileOpen->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data());
            }

            hr = pFileOpen->Show(nullptr);
            if (SUCCEEDED(hr)) {
                IShellItemArray* pItemArray = nullptr;
                hr = pFileOpen->GetResults(&pItemArray);
                if (SUCCEEDED(hr)) {
                    DWORD count = 0;
                    pItemArray->GetCount(&count);

                    std::vector<std::string> files;
                    files.reserve(count);
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* pItem = nullptr;
                        hr = pItemArray->GetItemAt(i, &pItem);
                        if (SUCCEEDED(hr)) {
                            PWSTR pszFilePath = nullptr;
                            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                            if (SUCCEEDED(hr) && pszFilePath) {
                                files.push_back(WStringToString(pszFilePath));
                                CoTaskMemFree(pszFilePath);
                            }
                            pItem->Release();
                        }
                    }
                    promise.set_value(files);
                    pItemArray->Release();
                }
            }
            pFileOpen->Release();
        }

        if (FAILED(hr)) {
            promise.set_value({});
        }

        return promise.get_future();
    }

    std::future<std::string> PlatformPickDirectory()
    {
        std::promise<std::string> promise;
        HRESULT hr;

        IFileOpenDialog* pFileOpen = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
        if (SUCCEEDED(hr)) {
            DWORD dwOptions;
            hr = pFileOpen->GetOptions(&dwOptions);
            if (SUCCEEDED(hr)) {
                hr = pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS);
            }

            hr = pFileOpen->Show(nullptr);
            if (SUCCEEDED(hr)) {
                IShellItem* pItem = nullptr;
                hr = pFileOpen->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFolderPath = nullptr;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFolderPath);
                    if (SUCCEEDED(hr) && pszFolderPath) {
                        promise.set_value(WStringToString(pszFolderPath));
                        CoTaskMemFree(pszFolderPath);
                    }
                    pItem->Release();
                }
            }
            pFileOpen->Release();
        }

        if (FAILED(hr)) {
            promise.set_value("");
        }

        return promise.get_future();
    }

    std::future<std::string> PlatformSaveFile(const std::string& default_name, const std::vector<std::pair<std::string, std::string>>& filters)
    {
        std::promise<std::string> promise;
        HRESULT hr;

        IFileSaveDialog* pFileSave = nullptr;
        hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileSave));
        if (SUCCEEDED(hr)) {
            if (!default_name.empty()) {
                std::wstring wdef = StringToWString(default_name);
                pFileSave->SetFileName(wdef.c_str());
            }

            std::vector<std::wstring> filterNames;
            std::vector<std::wstring> filterSpecs;
            filterNames.reserve(filters.size());
            filterSpecs.reserve(filters.size());

            for (const auto& filter : filters) {
                filterNames.push_back(StringToWString(filter.first));
                filterSpecs.push_back(StringToWString(filter.second));
            }

            std::vector<COMDLG_FILTERSPEC> fileTypes;
            fileTypes.reserve(filters.size());
            for (size_t i = 0; i < filters.size(); ++i) {
                fileTypes.push_back({ filterNames[i].c_str(), filterSpecs[i].c_str() });
            }

            if (!fileTypes.empty()) {
                pFileSave->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data());
            }

            hr = pFileSave->Show(nullptr);
            if (SUCCEEDED(hr)) {
                IShellItem* pItem = nullptr;
                hr = pFileSave->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath = nullptr;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr) && pszFilePath) {
                        promise.set_value(WStringToString(pszFilePath));
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
            pFileSave->Release();
        }

        if (FAILED(hr)) {
            promise.set_value("");
        }

        return promise.get_future();
    }
}
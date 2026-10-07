#include "widevine_util.h"

#include "ipc.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_component_updater.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

namespace shared
{

const char kWidevineComponentId[] = "oimompecagnajdejgnnjijobebaeigek";

#if defined(OS_LINUX)
const char kWidevineCdmPathSwitch[] = "widevine-cdm-path";
#endif

namespace
{

const char kWidevineCdmBaseDirectory[] = "WidevineCdm";
const char kHintFileName[] = "latest-component-updated-widevine-cdm";
// Chromium's kNullVersion (components/component_updater/component_installer.cc): the version of a component with nothing installed.
const char kNullComponentVersion[] = "0.0.0.0";

bool g_cdm_present_at_startup = false;
// Only the callback of the latest Update call publishes the state.
int g_update_generation = 0;

bool IsCdmDirectory(const std::filesystem::path& cdm_directory)
{
    if (cdm_directory.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(cdm_directory / "manifest.json", ec))
        return false;

#if defined(OS_LINUX)
#if defined(__aarch64__)
    const char* platform_directory = "linux_arm64";
#else
    const char* platform_directory = "linux_x64";
#endif
    return std::filesystem::exists(cdm_directory / "_platform_specific" / platform_directory / "libwidevinecdm.so", ec);
#else
    return std::filesystem::is_directory(cdm_directory / "_platform_specific", ec);
#endif
}

std::filesystem::path ReadHintedCdmDirectory(const std::filesystem::path& hint_file)
{
    std::ifstream file(hint_file);
    if (!file)
        return {};

    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string key = "\"Path\":\"";
    const auto start = contents.find(key);
    if (start == std::string::npos)
        return {};

    const auto value_start = start + key.length();
    const auto value_end = contents.find('"', value_start);
    if (value_end == std::string::npos)
        return {};

    return std::filesystem::path(contents.substr(value_start, value_end - value_start));
}

std::filesystem::path GetHintFilePath(const std::string& root_cache_path)
{
    return std::filesystem::path(root_cache_path) / kWidevineCdmBaseDirectory / kHintFileName;
}

bool IsCdmPresent(const std::string& root_cache_path)
{
    if (root_cache_path.empty())
        return false;

    if (IsCdmDirectory(ReadHintedCdmDirectory(GetHintFilePath(root_cache_path))))
        return true;

    const auto base_directory = std::filesystem::path(root_cache_path) / kWidevineCdmBaseDirectory;

    std::error_code ec;
    if (!std::filesystem::is_directory(base_directory, ec))
        return false;

    for (const auto& entry : std::filesystem::directory_iterator(base_directory, ec))
    {
        if (IsCdmDirectory(entry.path()))
            return true;
    }

    return false;
}

#if defined(OS_LINUX)
std::filesystem::path PathFromSwitch(const CefRefPtr<CefCommandLine>& command_line, const char* name)
{
    return std::filesystem::path(command_line->GetSwitchValue(name).ToString());
}

bool MaybeWriteHintFile(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path)
{
    if (!command_line->HasSwitch(kWidevineCdmPathSwitch) || root_cache_path.empty())
        return false;

    const auto cdm_directory = PathFromSwitch(command_line, kWidevineCdmPathSwitch);
    if (!IsCdmDirectory(cdm_directory))
    {
        LOG(WARNING) << "Ignoring --" << kWidevineCdmPathSwitch << "=" << cdm_directory.string() << " because it does not contain a usable Widevine CDM.";
        return false;
    }

    const auto hint_file = GetHintFilePath(root_cache_path);
    if (IsCdmDirectory(ReadHintedCdmDirectory(hint_file)))
    {
        LOG(INFO) << "Keeping the existing Widevine CDM hint file.";
        return false;
    }

    const std::string cdm_directory_value = cdm_directory.string();
    if (cdm_directory_value.find_first_of("\"\\") != std::string::npos)
    {
        LOG(WARNING) << "Ignoring Widevine CDM path containing quotes or backslashes.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(hint_file.parent_path(), ec);
    if (ec)
    {
        LOG(ERROR) << "Failed to create " << hint_file.parent_path().string() << ". Error: " << ec.message();
        return false;
    }

    const auto temporary_file = hint_file.string() + ".tmp";
    {
        std::ofstream file(temporary_file, std::ios::trunc);
        if (!file)
        {
            LOG(ERROR) << "Failed to write " << temporary_file;
            return false;
        }
        file << "{\"Path\":\"" << cdm_directory_value << "\"}";
    }

    std::filesystem::rename(temporary_file, hint_file, ec);
    if (ec)
    {
        LOG(ERROR) << "Failed to move " << temporary_file << " to " << hint_file.string() << ". Error: " << ec.message();
        std::filesystem::remove(temporary_file, ec);
        return false;
    }

    LOG(INFO) << "Pointed the Widevine CDM hint file at " << cdm_directory_value;
    return true;
}
#endif

bool IsInstalledState(cef_component_state_t state)
{
    return state == CEF_COMPONENT_STATE_UPDATED || state == CEF_COMPONENT_STATE_UP_TO_DATE || state == CEF_COMPONENT_STATE_RUN;
}

bool IsInProgressState(cef_component_state_t state)
{
    return state == CEF_COMPONENT_STATE_NEW || state == CEF_COMPONENT_STATE_CHECKING || state == CEF_COMPONENT_STATE_CAN_UPDATE
        || state == CEF_COMPONENT_STATE_DOWNLOADING || state == CEF_COMPONENT_STATE_DECOMPRESSING
        || state == CEF_COMPONENT_STATE_PATCHING || state == CEF_COMPONENT_STATE_UPDATING;
}

// Chromium registers the component with the newest CDM version on disk at startup, before any update check runs.
bool IsCdmInstalled(const CefRefPtr<CefComponent>& component)
{
    const std::string version = component->GetVersion();
    return IsInstalledState(component->GetState()) || (!version.empty() && version != kNullComponentVersion);
}

bool IsStatusKnownBeforeUpdate(const CefRefPtr<CefComponent>& component)
{
#if defined(OS_LINUX)
    // Linux loads the CDM from the hint file at startup, which can point outside the component directory.
    if (g_cdm_present_at_startup)
        return true;
#endif
    return IsCdmInstalled(component);
}

class WidevineUpdateCallback : public CefComponentUpdateCallback
{
public:
    static void Queue(CefRefPtr<CefComponentUpdater> updater)
    {
        updater->Update(kWidevineComponentId, CEF_COMPONENT_UPDATE_PRIORITY_FOREGROUND, new WidevineUpdateCallback(++g_update_generation));
    }

    void OnComplete(const CefString& component_id, cef_component_update_error_t error) override
    {
        if (_generation != g_update_generation)
        {
            LOG(INFO) << "Ignoring a Widevine CDM update callback superseded by a newer update.";
            return;
        }

        if (error != CEF_COMPONENT_UPDATE_ERROR_NONE)
        {
            LOG(WARNING) << "Widevine CDM update failed with error " << error;
            PublishWidevineStatus(WidevineUnavailableReason::UpdateFailed, "Widevine CDM update failed (error " + std::to_string(error) + ").");
            return;
        }

        // OnComplete runs after the next queued update task has started, so the component can report that task's state.
        // Queue another update and read the state when it completes.
        CefRefPtr<CefComponentUpdater> updater = CefComponentUpdater::GetComponentUpdater();
        CefRefPtr<CefComponent> component = updater ? updater->GetComponentByID(kWidevineComponentId) : nullptr;
        if (component && IsInProgressState(component->GetState()))
        {
            LOG(INFO) << "Widevine CDM update completed while another update is running (state " << component->GetState() << "), waiting for it.";
            WidevineUpdateCallback::Queue(updater);
            return;
        }

        PublishWidevineStatus();
    }

private:
    explicit WidevineUpdateCallback(int generation) : _generation(generation) {}

    int _generation;

    IMPLEMENT_REFCOUNTING(WidevineUpdateCallback);
    DISALLOW_COPY_AND_ASSIGN(WidevineUpdateCallback);
};

// The component registers shortly after the context, so retry often for up to 30 seconds.
const int kUpdateAttempts = 300;
const int64_t kUpdateRetryDelayMs = 100;

void RequestWidevineCdmUpdate(int attempt)
{
    CEF_REQUIRE_UI_THREAD();

    CefRefPtr<CefComponentUpdater> updater = CefComponentUpdater::GetComponentUpdater();
    if (!updater)
    {
        LOG(WARNING) << "Component updater unavailable, skipping the Widevine CDM update.";
        PublishWidevineStatus(WidevineUnavailableReason::UpdaterUnavailable, "The Widevine component updater is unavailable.");
        return;
    }

    CefRefPtr<CefComponent> component = updater->GetComponentByID(kWidevineComponentId);
    if (!component)
    {
        if (attempt + 1 < kUpdateAttempts)
        {
            CefPostDelayedTask(TID_UI, base::BindOnce(&RequestWidevineCdmUpdate, attempt + 1), kUpdateRetryDelayMs);
            return;
        }

        LOG(INFO) << "Widevine is not a registered component.";
        PublishWidevineStatus(WidevineUnavailableReason::NotSupported, "Widevine is not a registered component.");
        return;
    }

    if (IsStatusKnownBeforeUpdate(component))
    {
        LOG(INFO) << "Widevine CDM " << component->GetVersion().ToString() << " is available at startup (state " << component->GetState() << "), checking for a newer version in the background.";
        PublishWidevineStatus();
        // A newer version is installed next to the loaded one and only used after a restart.
        updater->Update(kWidevineComponentId, CEF_COMPONENT_UPDATE_PRIORITY_FOREGROUND, nullptr);
        return;
    }

    LOG(INFO) << "No Widevine CDM installed yet (version " << component->GetVersion().ToString() << ", state " << component->GetState() << "), waiting for the update.";
    WidevineUpdateCallback::Queue(updater);
}

} // namespace

void InitializeWidevineState(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path)
{
#if defined(OS_LINUX)
    MaybeWriteHintFile(command_line, root_cache_path);
#endif

    g_cdm_present_at_startup = IsCdmPresent(root_cache_path);
    LOG(INFO) << "Widevine CDM present at startup: " << g_cdm_present_at_startup;
}

void RequestWidevineCdmUpdate()
{
    RequestWidevineCdmUpdate(0);
}

void PublishWidevineStatus(WidevineUnavailableReason reason, const std::string& detail)
{
    CEF_REQUIRE_UI_THREAD();

    bool installed = false;
    std::string version;
    CefRefPtr<CefComponentUpdater> updater = CefComponentUpdater::GetComponentUpdater();
    CefRefPtr<CefComponent> component = updater ? updater->GetComponentByID(kWidevineComponentId) : nullptr;
    if (component)
    {
        installed = IsCdmInstalled(component);
        version = component->GetVersion();
    }

#if defined(OS_LINUX)
    const bool requiresRestart = installed && !g_cdm_present_at_startup;
    installed = installed || g_cdm_present_at_startup;
#else
    const bool requiresRestart = false;
#endif

    const WidevineStatus status = ResolveWidevineStatus(installed, requiresRestart, version, reason, detail);
    LOG(INFO) << "Widevine status: state=" << static_cast<int32_t>(status.state) << ", reason=" << static_cast<int32_t>(status.reason) << ", version=" << status.version;
    IPC::Singleton.NotifyWidevineStatus(status);
}

} // namespace shared

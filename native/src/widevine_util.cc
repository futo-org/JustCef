#include "widevine_util.h"

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

namespace shared {

  const char kWidevineComponentId[] = "oimompecagnajdejgnnjijobebaeigek";

#if defined(OS_LINUX)
  const char kWidevineCdmPathSwitch[] = "widevine-cdm-path";
#endif

  namespace {

    const char kWidevineCdmBaseDirectory[] = "WidevineCdm";
    const char kHintFileName[] = "latest-component-updated-widevine-cdm";

    bool g_cdm_present_at_startup = false;

    bool IsCdmDirectory(const std::filesystem::path& cdm_directory) {
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

    std::filesystem::path ReadHintedCdmDirectory(const std::filesystem::path& hint_file) {
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

    std::filesystem::path GetHintFilePath(const std::string& root_cache_path) {
      return std::filesystem::path(root_cache_path) / kWidevineCdmBaseDirectory / kHintFileName;
    }

    bool IsCdmPresent(const std::string& root_cache_path) {
      if (root_cache_path.empty())
        return false;

      if (IsCdmDirectory(ReadHintedCdmDirectory(GetHintFilePath(root_cache_path))))
        return true;

      const auto base_directory = std::filesystem::path(root_cache_path) / kWidevineCdmBaseDirectory;

      std::error_code ec;
      if (!std::filesystem::is_directory(base_directory, ec))
        return false;

      for (const auto& entry : std::filesystem::directory_iterator(base_directory, ec)) {
        if (IsCdmDirectory(entry.path()))
          return true;
      }

      return false;
    }

#if defined(OS_LINUX)
    std::filesystem::path PathFromSwitch(const CefRefPtr<CefCommandLine>& command_line, const char* name) {
      return std::filesystem::path(command_line->GetSwitchValue(name).ToString());
    }

    bool MaybeWriteHintFile(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path) {
      if (!command_line->HasSwitch(kWidevineCdmPathSwitch) || root_cache_path.empty())
        return false;

      const auto cdm_directory = PathFromSwitch(command_line, kWidevineCdmPathSwitch);
      if (!IsCdmDirectory(cdm_directory)) {
        LOG(WARNING) << "Ignoring --" << kWidevineCdmPathSwitch << "=" << cdm_directory.string()
                     << " because it does not contain a usable Widevine CDM.";
        return false;
      }

      const auto hint_file = GetHintFilePath(root_cache_path);
      if (IsCdmDirectory(ReadHintedCdmDirectory(hint_file))) {
        LOG(INFO) << "Keeping the existing Widevine CDM hint file.";
        return false;
      }

      const std::string cdm_directory_value = cdm_directory.string();
      if (cdm_directory_value.find_first_of("\"\\") != std::string::npos) {
        LOG(WARNING) << "Ignoring Widevine CDM path containing quotes or backslashes.";
        return false;
      }

      std::error_code ec;
      std::filesystem::create_directories(hint_file.parent_path(), ec);
      if (ec) {
        LOG(ERROR) << "Failed to create " << hint_file.parent_path().string() << ". Error: " << ec.message();
        return false;
      }

      const auto temporary_file = hint_file.string() + ".tmp";
      {
        std::ofstream file(temporary_file, std::ios::trunc);
        if (!file) {
          LOG(ERROR) << "Failed to write " << temporary_file;
          return false;
        }
        file << "{\"Path\":\"" << cdm_directory_value << "\"}";
      }

      std::filesystem::rename(temporary_file, hint_file, ec);
      if (ec) {
        LOG(ERROR) << "Failed to move " << temporary_file << " to " << hint_file.string() << ". Error: " << ec.message();
        std::filesystem::remove(temporary_file, ec);
        return false;
      }

      LOG(INFO) << "Pointed the Widevine CDM hint file at " << cdm_directory_value;
      return true;
    }
#endif

    bool IsInstalledState(cef_component_state_t state) {
      return state == CEF_COMPONENT_STATE_UPDATED ||
             state == CEF_COMPONENT_STATE_UP_TO_DATE ||
             state == CEF_COMPONENT_STATE_RUN;
    }

    class WidevineUpdateCallback : public CefComponentUpdateCallback {
     public:
      WidevineUpdateCallback() {}

      void OnComplete(const CefString& component_id, cef_component_update_error_t error) override {
        if (error != CEF_COMPONENT_UPDATE_ERROR_NONE) {
          LOG(WARNING) << "Widevine CDM update failed with error " << error;
          return;
        }

        const WidevineStatus status = GetWidevineStatus();
        LOG(INFO) << "Widevine CDM update completed, installed=" << status.installed
                  << ", version=" << status.version
                  << ", requiresRestart=" << status.requiresRestart;
      }

     private:
      IMPLEMENT_REFCOUNTING(WidevineUpdateCallback);
      DISALLOW_COPY_AND_ASSIGN(WidevineUpdateCallback);
    };

    const int kUpdateAttempts = 15;
    const int64_t kUpdateRetryDelayMs = 2000;

    void RequestWidevineCdmUpdate(int attempt) {
      CEF_REQUIRE_UI_THREAD();

      CefRefPtr<CefComponentUpdater> updater = CefComponentUpdater::GetComponentUpdater();
      if (!updater) {
        LOG(WARNING) << "Component updater unavailable, skipping the Widevine CDM update.";
        return;
      }

      CefRefPtr<CefComponent> component = updater->GetComponentByID(kWidevineComponentId);
      if (!component) {
        if (attempt + 1 < kUpdateAttempts) {
          CefPostDelayedTask(TID_UI, base::BindOnce(&RequestWidevineCdmUpdate, attempt + 1), kUpdateRetryDelayMs);
          return;
        }

        LOG(INFO) << "Widevine is not a registered component.";
        return;
      }

      if (IsInstalledState(component->GetState())) {
        LOG(INFO) << "Widevine CDM " << component->GetVersion().ToString() << " is already installed.";
        return;
      }

      updater->Update(kWidevineComponentId, CEF_COMPONENT_UPDATE_PRIORITY_FOREGROUND, new WidevineUpdateCallback());
    }

  }

  void InitializeWidevineState(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path) {
#if defined(OS_LINUX)
    MaybeWriteHintFile(command_line, root_cache_path);
#endif

    g_cdm_present_at_startup = IsCdmPresent(root_cache_path);
    LOG(INFO) << "Widevine CDM present at startup: " << g_cdm_present_at_startup;
  }

  void RequestWidevineCdmUpdate() {
    RequestWidevineCdmUpdate(0);
  }

  WidevineStatus GetWidevineStatus() {
    WidevineStatus status;

    CefRefPtr<CefComponentUpdater> updater = CefComponentUpdater::GetComponentUpdater();
    if (!updater)
      return status;

    CefRefPtr<CefComponent> component = updater->GetComponentByID(kWidevineComponentId);
    if (!component)
      return status;

    const cef_component_state_t state = component->GetState();

    status.registered = true;
    status.installed = IsInstalledState(state);
    status.state = static_cast<int32_t>(state);
    status.version = component->GetVersion();

#if defined(OS_LINUX)
    status.requiresRestart = status.installed && !g_cdm_present_at_startup;
#else
    status.requiresRestart = false;
#endif

    return status;
  }

}

#include "app_factory.h"
#include "custom_schemes.h"
namespace shared
{
class OtherApp : public CefApp
{
public:
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { RegisterCustomSchemes(registrar); }

private:
    IMPLEMENT_REFCOUNTING(OtherApp);
};
CefRefPtr<CefApp> CreateOtherProcessApp()
{
    return new OtherApp();
}
} // namespace shared

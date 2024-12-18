#include "devtoolsclient.h"

DevToolsClient::DevToolsClient() {}

bool DevToolsClient::OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event)
{
    if (event.type == KEYEVENT_RAWKEYDOWN)
    {
        switch (event.windows_key_code)
        {
            case 0x7B: //F12
                browser->GetHost()->CloseBrowser(false);
                return false;
                
        }
    }
    return false;
}


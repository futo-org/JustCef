#ifndef CEF_DOTCEF_MAIN_H_
#define CEF_DOTCEF_MAIN_H_

#include "include/base/cef_build.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace shared {

    // Entry point function shared by executable targets.
    #if defined(OS_WIN)
    int APIENTRY wWinMain(HINSTANCE hInstance);
    #else
    int main(int argc, char* argv[]);
    #endif

}
#endif  // CEF_DOTCEF_MAIN_H_

// Mac entrypoint. axmol's CMake template expects it at this exact path.
// Delegates to the shared AppDelegate under app/, passing through any .csd
// path given as argv[1].

#include "AppDelegate.h"
#include "axmol.h"

#include <filesystem>

int main(int argc, char* argv[])
{
    // argv parsing:
    //   --panel=<name>   detached-panel mode (Scene|Assets|Properties|Layout|Mode)
    //   <path>           a .csd to open as a tab (multi-arg allowed)
    // Flags go through AppDelegate statics; paths go into sCsdPaths.
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a(argv[i]);
        if (a.rfind("--panel=", 0) == 0)
        {
            AppDelegate::sPanelMode.assign(a.substr(8));
            continue;
        }
        if (a.rfind("--size=", 0) == 0)
        {
            // Format: --size=WxH (integers, no spaces). Parent window
            // passes this when detaching a panel so the new OS window
            // opens at the exact panel size.
            std::string rest(a.substr(7));
            auto xpos = rest.find('x');
            if (xpos != std::string::npos)
            {
                AppDelegate::sPanelWindowW = std::atoi(rest.substr(0, xpos).c_str());
                AppDelegate::sPanelWindowH = std::atoi(rest.substr(xpos + 1).c_str());
            }
            continue;
        }
        AppDelegate::sCsdPaths.push_back(
            std::filesystem::absolute(argv[i]).string());
    }
    // Back-compat: expose argv[1] as sCsdPath so code that still reads
    // the single-path field (older tooling, tests) sees the first scene.
    if (!AppDelegate::sCsdPaths.empty())
        AppDelegate::sCsdPath = AppDelegate::sCsdPaths.front();

    AppDelegate app;
    auto result = ax::Application::getInstance()->run();

#if AX_OBJECT_LEAK_DETECTION
    ax::Object::printLeaks();
#endif
    return result;
}

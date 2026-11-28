/*---------------------------------------------------------*\
|| fake_windeployqt -- offline stand-in for Qt's            ||
|| windeployqt, used by package_stage_release_test to run   ||
|| the REAL stage-release.ps1 without a Qt kit. It accepts  ||
|| any arguments, treats the last one as the deployed exe   ||
|| and drops placeholder runtime files next to it -- just   ||
|| the members stage-release.ps1's verification requires.   ||
|| With --qmldir it also creates qml/.                      ||
\*---------------------------------------------------------*/

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void Touch(const fs::path& p)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "fake windeployqt output\n";
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        return 2;
    }
    bool qml = false;
    for(int i = 1; i < argc - 1; ++i)
    {
        if(std::string(argv[i]) == "--qmldir")
        {
            qml = true;
        }
    }
    const fs::path dir = fs::path(argv[argc - 1]).parent_path();
    for(const char* f : { "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                          "Qt6QuickControls2.dll", "platforms/qwindows.dll" })
    {
        Touch(dir / f);
    }
    if(qml)
    {
        Touch(dir / "qml" / "QtQuick" / "qmldir");
    }
    return 0;
}

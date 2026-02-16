#include <windows.h>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
    if (argc < 3)
        return 1;

    fs::path oldExe = argv[1];
    fs::path newExe = argv[2];

    // Try for up to ~15 seconds
    const int attempts = 150;
    for (int i = 0; i < attempts; ++i) {
        // Try replace
        if (MoveFileExA(
            newExe.string().c_str(),
            oldExe.string().c_str(),
            MOVEFILE_REPLACE_EXISTING))
        {
            // Restart
            ShellExecuteA(nullptr, "open", oldExe.string().c_str(),
                nullptr, oldExe.parent_path().string().c_str(), SW_SHOWNORMAL);
            return 0;
        }

        // If it failed, wait a bit and try again
        Sleep(100);
    }

    // Still failed
    return 2;
}

#include <iostream>
#include <filesystem>  // C++17
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <getopt.h>
#endif

#include "md.h"
#include "helptext.h"

namespace fs = std::filesystem;
using namespace std;

static const std::string PROGRAM_VERSION = "T1.0.5";

static void print_version() {
    std::cout << "MotionDynamics Version: " << PROGRAM_VERSION << std::endl;
}

static void print_help() {
    std::cout << HELP_TEXT << std::endl;
}

static fs::path get_root(char* argv0) {
    fs::path exePath = fs::absolute(argv0);      // /ROOTPATH/build/LIS
    fs::path exeDir = exePath.parent_path();    // /ROOTPATH/build
    fs::path rootDir = exeDir.parent_path();    // /ROOTPATH
    return rootDir;
}

static void run_update(char* argv0)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) {
        std::cout << "[UPDATE] Cannot determine executable path.\n";
        return;
    }
    fs::path exe_path = fs::path(buf);
    fs::path exe_dir = exe_path.parent_path();

    fs::path new_exe = exe_dir / "MotionDynamics_4TO_new.exe";
    fs::path updater = exe_dir / "MotionDynamics_4TO_updater.exe";

    if (!fs::exists(updater)) {
        std::cout << "[UPDATE] Missing updater: " << updater << "\n";
        return;
    }

    std::cout << "[UPDATE] Downloading new version...\n";

    const std::string url =
        "https://github.com/CiaranWang/MotionDynamics_4TO/releases/latest/download/MotionDynamics_4TO.exe";

    std::string cmd =
        "where curl >nul 2>nul"
        " && curl -fL -o \"" + new_exe.string() + "\" \"" + url + "\""
        " || powershell -NoProfile -Command \""
        "try { Invoke-WebRequest -Uri \\\"" + url + "\\\" -OutFile \\\"" + new_exe.string() + "\\\" -UseBasicParsing }"
        "catch { exit 1 }\"";

    if (system(cmd.c_str()) != 0 || !fs::exists(new_exe)) {
        std::cout << "[UPDATE] Download failed.\n";
        return;
    }

    std::cout << "[UPDATE] Launching updater...\n";

    std::string args =
        "\"" + exe_path.string() + "\" \"" + new_exe.string() + "\"";

    ShellExecuteA(
        nullptr,
        "open",
        updater.string().c_str(),
        args.c_str(),
        exe_dir.string().c_str(),
        SW_SHOWNORMAL
    );

    std::exit(0); // Must exit so updater can replace the exe
#else
    fs::path md_root = get_root(argv0);

    std::cout << "[UPDATE] Attempting to update MotionDynamics_4TO in: " << md_root << std::endl;
    std::cout << "Make sure you have 'git', 'cmake', and 'make' installed." << std::endl;

    // Check if .git folder exists
    if (!fs::exists(md_root / ".git") || !fs::is_directory(md_root / ".git")) {
        std::cout << "Warning: LIS root folder is not a git repository.\n";
        std::cout << "Clone the repository and try again:\n";
        std::cout << "  git clone https://github.com/CiaranWang/MotionDynamics_4TO.git\n";
        return;
    }

    // Pull latest changes
    std::string git_cmd = "cd \"" + md_root.string() + "\" && git pull origin master";
    if (system(git_cmd.c_str()) != 0) {
        std::cout << "Git pull failed.\n";
        return;
    }

    // Build project
    std::string build_cmd =
        "cd \"" + md_root.string() + "\" && mkdir -p build && cd build && cmake .. && make -j 8";
    if (system(build_cmd.c_str()) != 0) {
        std::cout << "Build failed.\n";
        return;
    }

    std::cout << "Update and rebuild completed successfully!\n";
    std::cout << "You can now run: ./build/MotionDynamics_4TO [options]\n";
#endif
}

int main(int argc, char* argv[]) 
{
    // ================================================================
    // Early check for --version / --update / --help
    // ================================================================
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--version" || arg1 == "-v" || arg1 == "-V") {
            print_version();
            return 0;
        }
        if (arg1 == "--update" || arg1 == "-u" || arg1 == "-U") {
            run_update(argv[0]);
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "-H") {
            print_help();
            return 0;
        }
    }
    // ================================================================

    std::filesystem::path input_file;
    std::filesystem::path output_dir;
    long frame_window = 200;
    long min_len = 0;
    bool track_mode = false;
    bool cal_pheno_mode = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--track") {
            track_mode = true;
        }
        else if (arg == "--cal_pheno") {
            cal_pheno_mode = true;
        }
        else if (arg == "-i" && i + 1 < argc) {
            input_file = argv[++i];
        }
        else if (arg == "-o" && i + 1 < argc) {
            output_dir = argv[++i];
        }
        else if (arg == "--window" && i + 1 < argc) {
            try {
                frame_window = std::stol(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --window must be an integer.\n";
                return 1;
            }
            if (frame_window <= 0) {
                std::cerr << "Error: --window must be > 0.\n";
                return 1;
            }
        }
        else if (arg == "--min_len" && i + 1 < argc) {
            try {
                min_len = std::stol(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --min_len must be an integer.\n";
                return 1;
            }
            if (min_len <= 0) {
                std::cerr << "Error: --min_len must be > 0.\n";
                return 1;
            }
        }
        else {
            std::cerr << "Error: unknown or incomplete argument: " << arg << "\n";
            std::cerr << "Use --help for usage.\n";
            return 1;
        }      
    }

    if (!track_mode && !cal_pheno_mode) {
        std::cerr << "Error: please selece at leaset one mode with --track or --cal_pheno\n";
        return 1;
    }

    else if (track_mode && cal_pheno_mode) {
        std::cout << "Warning: You are going to calculate phenotypes right after generation of tracks.\n";
        std::cout << "So tracks are not filtered, make sure this is what you want.\n";
    }

    if (track_mode)
    {
        if (input_file.empty()) {
            std::cerr << "Error: no input file (-i)\n";
            return 1;
        }

        if (output_dir.empty()) {
            output_dir = input_file.parent_path();
        }

        // Ensure the output folder exists
        if (!fs::exists(output_dir)) {
            try {
                fs::create_directories(output_dir); // creates all missing intermediate directories
                std::cout << "[INFO] Created output folder: " << output_dir << std::endl;
            }
            catch (const fs::filesystem_error& e) {
                std::cerr << "Error: Failed to create output folder '"
                    << output_dir << "': " << e.what() << std::endl;
                return 1;
            }
        }

        int n_tracks = get_tracks(input_file, output_dir, frame_window, min_len);
    }

    return 0;
 
}
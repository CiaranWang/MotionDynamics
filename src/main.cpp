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
#include "ReadInput.h"

namespace fs = std::filesystem;
using namespace std;

static const std::string PROGRAM_VERSION = "3.1.2";

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

    fs::path new_exe = exe_dir / "MotionDynamics_new.exe";
    fs::path updater = exe_dir / "MotionDynamics_updater.exe";

    if (!fs::exists(updater)) {
        std::cout << "[UPDATE] Missing updater: " << updater << "\n";
        return;
    }

    std::cout << "[UPDATE] Downloading new version...\n";

    const std::string url =
        "https://github.com/CiaranWang/MotionDynamics/releases/latest/download/MotionDynamics.exe";

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

    std::cout << "[UPDATE] Attempting to update MotionDynamics in: " << md_root << std::endl;
    std::cout << "Make sure you have 'git', 'cmake', and 'make' installed." << std::endl;

    // Check if .git folder exists
    if (!fs::exists(md_root / ".git") || !fs::is_directory(md_root / ".git")) {
        std::cout << "Warning: MotionDynamics root folder is not a git repository.\n";
        std::cout << "Clone the repository and try again:\n";
        std::cout << "  git clone https://github.com/CiaranWang/MotionDynamics.git\n";
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
    std::cout << "You can now run: ./build/MotionDynamics [options]\n";
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
    std::filesystem::path parameter_file;
    std::filesystem::path output_dir;
    long frame_window = 200;
    long min_len = 0;
    double noise_dist = 0.0;
    double max_speed = 0.0;
    int local_window = 2;
    double local_dist = 0.0;
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
        else if (arg == "-p" && i + 1 < argc) {
            parameter_file = argv[++i];
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
        else if (arg == "--noise_dist" && i + 1 < argc) {
            try {
                noise_dist = std::stod(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --noise_dist must be a number.\n";
                return 1;
            }
            if (noise_dist < 0.0) {
                std::cerr << "Error: --noise_dist must be >= 0.\n";
                return 1;
            }
        }
        else if (arg == "--max_speed" && i + 1 < argc) {
            try {
                max_speed = std::stod(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --max_speed must be a number.\n";
                return 1;
            }
            if (max_speed < 0.0) {
                std::cerr << "Error: --max_speed must be >= 0.\n";
                return 1;
            }
        }
        else if (arg == "--local_window" && i + 1 < argc) {
            try {
                local_window = std::stoi(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --local_window must be an integer.\n";
                return 1;
            }
            if (local_window < 1) {
                std::cerr << "Error: --local_window must be >= 1.\n";
                return 1;
            }
        }
        else if (arg == "--local_dist" && i + 1 < argc) {
            try {
                local_dist = std::stod(argv[++i]);
            }
            catch (const std::exception&) {
                std::cerr << "Error: --local_dist must be a number.\n";
                return 1;
            }
            if (local_dist < 0.0) {
                std::cerr << "Error: --local_dist must be >= 0.\n";
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
        std::cerr << "ERROR:you cannot run both track mode and phenotyping mode at the same time!\n";
        std::cerr << "Use either --track or --cal_pheno. Do not use both.\n";
        return 1;
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

        int n_tracks = get_tracks(input_file, output_dir, frame_window, min_len,
            noise_dist, max_speed, local_window, local_dist);
    }

    else if (cal_pheno_mode)
    {
        if (input_file.empty()) {
            std::cerr << "Error: no input file (-i)\n";
            return 1;
        }

        if (parameter_file.empty()) {
            std::cerr << "Warning: no parameter file (-p). Will use default values\n\n";
            std::cout << "Warning: no parameter file (-p). Will use default values\n\n";
        }

        if (load_parameters(parameter_file.string())){
            cout << "Parameter read successfully:\n";
        }
        else {
            cout << "Warning: Parameter read FAILED, using default values:\n";
        }

        print_parameters();

        calculate_phenotype(input_file, output_dir);
    }
    return 0;
}

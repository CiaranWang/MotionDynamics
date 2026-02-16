#ifndef MD_H
#define MD_H

#include <string>
#include <vector>
#include <filesystem>  // C++17

constexpr double kPI = 3.14159265358979323846;

// ----------------------
// Detection of one animal at one frame
// ----------------------
struct Detection {
    int ID;
    long custom_frame;
    double cen_x;
    double cen_y;
    double dir_x;
    double dir_y;
    double angle;
};

// ----------------------
// Segment of continuous tracking for one animal
// ----------------------
struct Segment {
    std::vector<Detection> points;
};

// ----------------------
// Track summary over all segments
// ----------------------
struct TrackSummary {
    int ID;
    int segment_index;
    std::string unique_index;
    long first_frame;
    long last_frame;
    long frame_number;
    long n_obs;
    long max_gap;

    double total_distance;
    double max_jump;
};

// ----------------------
// Main engine entry point
// ----------------------
int motion_dynamics_run(const std::string& input,
    const std::string& output_prefix,
    long frame_window,
    bool smooth = false);

int get_tracks(const std::filesystem::path& input,
    const std::filesystem::path& output_dir,
    long frame_window,
    long min_len);

#endif
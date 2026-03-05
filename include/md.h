#ifndef MD_H
#define MD_H

#include <string>
#include <vector>
#include <filesystem>  // C++17

constexpr double kPI = 3.14159265358979323846;

// ----------------------
// Detection of one animal at one frame
// ----------------------
struct VideoTime {
    int hour = 0;
    int minute = 0;
    int second = 0;
};

struct Detection {
    int ID;
    long custom_frame;

    int pen;
    int day;
    VideoTime timestamp;

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
    std::string unique_track_id;
    std::string track_file;

    int ID = 0;

    long first_frame = 0;
    long last_frame = 0;
    long length = 0;

    int n_obs = 0;
    long max_gap = 0;

    double d_begin2end = 0.0;
    double d_accumulate = 0.0;
    double max_jump = 0.0;

    int start_pen = 0;
    int start_day = 0;
    VideoTime start_time{};

    int end_pen = 0;
    int end_day = 0;
    VideoTime end_time{};
};

struct DetRow {
    long frame = 0;
    int ID = 0;
    int pen = 0;
    int day = 0;
    VideoTime ts{};
    double x = 0.0;
    double y = 0.0;
    double angle = 0.0;
};

// what we will output per frame per ID
struct FrameObs {
    long frame = 0;
    int ID = 0;
    bool observed = false;   // true if from file, false if interpolated
    double x = 0.0, y = 0.0;
    VideoTime ts{};
    int pen = 0;
    int day = 0;
};

struct TrackInterval {
    int ID = 0;
    long first = 0;
    long last = 0;
    std::string unique_track_id;
    std::string track_file;
};

struct Event {
    long frame = 0;
    bool is_start = true; // start or end
    int ID = 0;
    size_t interval_index = 0;
};

struct TraitsPerInd {
    long frame = 0;
    int ID = 0;

    // 1) count within r1
    int n_within_r1 = 0;

    // 2) mean distance within r2 (only if n>0)
    int n_within_r2 = 0;
    double mean_dist_r2 = 0.0;

    // 3) proximity intensity score within r3
    double prox_intensity_r3 = 0.0;

    // 5) personal space weighted proximity (within r5_out)
    double personal_space_r5 = 0.0;
};

// 4) per-pair per-frame marker
struct PairWithin {
    long frame = 0;
    VideoTime ts{};
    int ID1 = 0;
    int observed1 = 0;
    int ID2 = 0;
    int observed2 = 0;
    int pen = 0;
    int day = 0;
    double dist = 0.0;
    int within_r4 = 0;
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

void calculate_phenotype(const std::filesystem::path& track_summary_csv, 
    const std::filesystem::path& out_csv);

#endif
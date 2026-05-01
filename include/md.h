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
    bool observed = false;

    double x = 0.0;
    double y = 0.0;

    double dir_x = 0.0;
    double dir_y = 0.0;

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

    double density_r = 0.0;
    double front_density = 0.0;
    double back_density = 0.0;
    double front_minus_back = 0.0;
};

struct GlobalTimePoint {
    long frame = 0;
    long sec = 0;   // seconds since midnight
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

struct HourlyIndAccum {
    long n_frames = 0;

    double sum_speed = 0.0;
    long n_speed_valid = 0;
    long n_moving = 0;

    double sum_density_r = 0.0;
    double sum_front_density = 0.0;
    double sum_back_density = 0.0;
    double sum_front_minus_back = 0.0;

    VideoTime start_ts{};
    VideoTime end_ts{};
    bool has_time = false;
};

struct HourlyPairAccum {
    long n_frames = 0;

    double sum_dist = 0.0;
    double sum_within_r4 = 0.0;

    VideoTime start_ts{};
    VideoTime end_ts{};
    bool has_time = false;
};

static int time_to_sec(const VideoTime& t)
{
    return t.hour * 3600 + t.minute * 60 + t.second;
}

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
    long min_len,
    double noise_dist = 0.0,
    double max_speed = 0.0,
    int local_window = 2,
    double local_dist = 0.0);

void calculate_phenotype(const std::filesystem::path& track_summary_csv, 
    const std::filesystem::path& out_csv);

#endif

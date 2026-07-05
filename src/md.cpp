#include "md.h"
#include "ReadInput.h"

#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <limits>
#include <cstddef>
#include <deque>

namespace fs = std::filesystem;
using namespace std;

constexpr double sigma = 100;

static inline double euclid(double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

static bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static long time_to_seconds(const VideoTime& t) {
    return 3600L * t.hour + 60L * t.minute + t.second;
}

static VideoTime seconds_to_time(long sec) {
    sec %= 86400L;
    if (sec < 0) sec += 86400L;
    VideoTime t{};
    t.hour = static_cast<int>(sec / 3600L);
    sec %= 3600L;
    t.minute = static_cast<int>(sec / 60L);
    t.second = static_cast<int>(sec % 60L);
    return t;
}

static VideoTime frame_to_time(long frame, double fps)
{
    if (fps <= 0.0) fps = 30.0;

    long sec = static_cast<long>(std::floor(static_cast<double>(frame) / fps));

    sec %= 86400L;
    if (sec < 0) sec += 86400L;

    VideoTime t{};
    t.hour = static_cast<int>(sec / 3600L);
    sec %= 3600L;
    t.minute = static_cast<int>(sec / 60L);
    t.second = static_cast<int>(sec % 60L);

    return t;
}

static long unwrap_to_near(long sec, long ref)
{
    // Move sec by +/- 86400 so it is closest to ref
    while (sec - ref > 43200L) sec -= 86400L;
    while (sec - ref < -43200L) sec += 86400L;
    return sec;
}

static long median_long(std::vector<long> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return static_cast<long>(std::llround((v[n / 2 - 1] + v[n / 2]) / 2.0));
}

static std::vector<GlobalTimePoint> build_global_time_points(
    const std::unordered_map<int, std::vector<DetRow>>& id_to_dets)
{
    std::unordered_map<long, std::vector<long>> frame_to_secs;

    // collect all observed timestamps from all IDs
    for (const auto& kv : id_to_dets) {
        const auto& dets = kv.second;
        for (const auto& d : dets) {
            frame_to_secs[d.frame].push_back(time_to_seconds(d.ts));
        }
    }

    // sort frames
    std::vector<long> frames;
    frames.reserve(frame_to_secs.size());
    for (const auto& kv : frame_to_secs) {
        frames.push_back(kv.first);
    }
    std::sort(frames.begin(), frames.end());

    std::vector<GlobalTimePoint> out;
    out.reserve(frames.size());

    long prev_sec_unwrapped = 0;
    bool first = true;

    for (long f : frames) {
        auto secs = frame_to_secs[f];

        // unwrap all seconds to be near previous point, to handle midnight nicely
        if (!first) {
            for (auto& s : secs) {
                s = unwrap_to_near(s, prev_sec_unwrapped);
            }
        }

        long sec_rep = median_long(secs);

        if (first) {
            prev_sec_unwrapped = sec_rep;
            first = false;
        }
        else {
            sec_rep = unwrap_to_near(sec_rep, prev_sec_unwrapped);
            prev_sec_unwrapped = sec_rep;
        }

        out.push_back(GlobalTimePoint{ f, sec_rep });
    }

    return out;
}

static VideoTime interpolate_between_points(
    long frame,
    long f0, long sec0,
    long f1, long sec1)
{
    if (f0 == f1) return seconds_to_time(sec0);

    const double w = static_cast<double>(frame - f0) / static_cast<double>(f1 - f0);
    const double sec_interp = static_cast<double>(sec0)
        + w * static_cast<double>(sec1 - sec0);

    return seconds_to_time(static_cast<long>(std::llround(sec_interp)));
}

static std::vector<std::vector<Detection>>
build_tracks_per_id(const std::vector<Detection>& detections,
                                long frame_window,
                                long min_len)
{
    std::vector<std::vector<Detection>> tracks;
    if (detections.empty()) return tracks;

    std::vector<Detection> cur;
    cur.reserve(1024);

    cur.push_back(detections[0]);

    for (size_t i = 1; i < detections.size(); ++i) {
        const long gap = detections[i].custom_frame - detections[i - 1].custom_frame;

        if (gap <= frame_window) {
            cur.push_back(detections[i]);
        }
        else {
            if ((long)cur.size() >= min_len) {
                tracks.push_back(std::move(cur));
            }
            cur.clear();
            cur.reserve(1024);
            cur.push_back(detections[i]);
        }
    }

    if ((long)cur.size() >= min_len) {
        tracks.push_back(std::move(cur));
    }

    return tracks;
}

static std::vector<Detection> filter_single_point_noise(
    const std::vector<Detection>& detections,
    long frame_window,
    double noise_dist,
    int& removed_count)
{
    removed_count = 0;

    if (noise_dist <= 0.0 || detections.size() < 3) {
        return detections;
    }

    std::vector<char> remove(detections.size(), 0);

    for (size_t i = 1; i + 1 < detections.size(); ++i) {
        const Detection& prev = detections[i - 1];
        const Detection& cur = detections[i];
        const Detection& next = detections[i + 1];

        const long gap_prev_cur = cur.custom_frame - prev.custom_frame;
        const long gap_cur_next = next.custom_frame - cur.custom_frame;
        const long gap_bridge = next.custom_frame - prev.custom_frame;

        const bool frames_are_bridgeable =
            gap_prev_cur > 0 &&
            gap_cur_next > 0 &&
            gap_bridge <= frame_window;

        if (frames_are_bridgeable) {
            const double d_prev_cur = euclid(prev.cen_x, prev.cen_y, cur.cen_x, cur.cen_y);
            const double d_cur_next = euclid(cur.cen_x, cur.cen_y, next.cen_x, next.cen_y);
            const double d_prev_next = euclid(prev.cen_x, prev.cen_y, next.cen_x, next.cen_y);

            const bool single_point_spike =
                d_prev_next <= noise_dist &&
                d_prev_cur > noise_dist &&
                d_cur_next > noise_dist;

            if (single_point_spike) {
                remove[i] = 1;
                ++removed_count;
            }
        }
    }

    std::vector<Detection> cleaned;
    cleaned.reserve(detections.size() - static_cast<size_t>(removed_count));

    for (size_t i = 0; i < detections.size(); ++i) {
        if (!remove[i]) cleaned.push_back(detections[i]);
    }

    return cleaned;
}

static std::vector<Detection> filter_speed_noise(
    const std::vector<Detection>& detections,
    long frame_window,
    double max_speed,
    int& removed_count)
{
    removed_count = 0;

    if (max_speed <= 0.0 || detections.size() < 3) {
        return detections;
    }

    std::vector<char> remove(detections.size(), 0);

    for (size_t i = 1; i + 1 < detections.size(); ++i) {
        const Detection& prev = detections[i - 1];
        const Detection& cur = detections[i];
        const Detection& next = detections[i + 1];

        const long gap_prev_cur = cur.custom_frame - prev.custom_frame;
        const long gap_cur_next = next.custom_frame - cur.custom_frame;
        const long gap_bridge = next.custom_frame - prev.custom_frame;

        const bool frames_are_bridgeable =
            gap_prev_cur > 0 &&
            gap_cur_next > 0 &&
            gap_bridge <= frame_window;

        if (frames_are_bridgeable) {
            const double speed_prev_cur =
                euclid(prev.cen_x, prev.cen_y, cur.cen_x, cur.cen_y)
                / static_cast<double>(gap_prev_cur);

            const double speed_cur_next =
                euclid(cur.cen_x, cur.cen_y, next.cen_x, next.cen_y)
                / static_cast<double>(gap_cur_next);

            const double speed_prev_next =
                euclid(prev.cen_x, prev.cen_y, next.cen_x, next.cen_y)
                / static_cast<double>(gap_bridge);

            const bool impossible_speed_spike =
                speed_prev_cur > max_speed &&
                speed_cur_next > max_speed &&
                speed_prev_next <= max_speed;

            if (impossible_speed_spike) {
                remove[i] = 1;
                ++removed_count;
            }
        }
    }

    std::vector<Detection> cleaned;
    cleaned.reserve(detections.size() - static_cast<size_t>(removed_count));

    for (size_t i = 0; i < detections.size(); ++i) {
        if (!remove[i]) cleaned.push_back(detections[i]);
    }

    return cleaned;
}

static std::vector<Detection> filter_local_trend_noise(
    const std::vector<Detection>& detections,
    long frame_window,
    int local_window,
    double local_dist,
    int& removed_count)
{
    removed_count = 0;

    if (local_dist <= 0.0 || local_window < 1 || detections.size() < 3) {
        return detections;
    }

    std::vector<Detection> cleaned = detections;

    for (;;) {
        std::vector<char> remove(cleaned.size(), 0);
        int removed_this_pass = 0;

        for (size_t i = 1; i + 1 < cleaned.size(); ++i) {
            const Detection& cur = cleaned[i];

            const Detection* left = nullptr;
            const Detection* right = nullptr;
            long best_gap = 0;

            const size_t left_begin =
                (i > static_cast<size_t>(local_window))
                ? i - static_cast<size_t>(local_window)
                : 0;

            const size_t right_end = std::min(
                cleaned.size() - 1,
                i + static_cast<size_t>(local_window));

            for (size_t li = left_begin; li < i; ++li) {
                if (cleaned[li].custom_frame >= cur.custom_frame) continue;

                for (size_t ri = i + 1; ri <= right_end; ++ri) {
                    if (cleaned[ri].custom_frame <= cur.custom_frame) continue;

                    const long gap_bridge = cleaned[ri].custom_frame - cleaned[li].custom_frame;
                    if (gap_bridge <= 0 || gap_bridge > frame_window) continue;

                    if (gap_bridge > best_gap) {
                        left = &cleaned[li];
                        right = &cleaned[ri];
                        best_gap = gap_bridge;
                    }
                }
            }

            if (left == nullptr || right == nullptr) continue;

            const double w =
                static_cast<double>(cur.custom_frame - left->custom_frame)
                / static_cast<double>(best_gap);

            const double expected_x = left->cen_x + w * (right->cen_x - left->cen_x);
            const double expected_y = left->cen_y + w * (right->cen_y - left->cen_y);
            const double deviation = euclid(cur.cen_x, cur.cen_y, expected_x, expected_y);

            if (deviation > local_dist) {
                remove[i] = 1;
                ++removed_this_pass;
            }
        }

        if (removed_this_pass == 0) break;

        std::vector<Detection> next;
        next.reserve(cleaned.size() - static_cast<size_t>(removed_this_pass));

        for (size_t i = 0; i < cleaned.size(); ++i) {
            if (!remove[i]) next.push_back(cleaned[i]);
        }

        removed_count += removed_this_pass;
        cleaned.swap(next);
    }

    return cleaned;
}

static void ensure_summary_header_if_needed(const fs::path& summary_file) {
    std::error_code ec;
    const bool exists = fs::exists(summary_file, ec);
    const bool empty = (!exists) || (fs::file_size(summary_file, ec) == 0);

    if (empty) {
        std::ofstream fout(summary_file, std::ios::out);
        if (!fout.is_open()) {
            std::cerr << "!!! Cannot create summary file: " << summary_file << "\n";
            return;
        }
        fout
            << "unique_track_id,track_file,ID,first_frame,last_frame,length,n_obs,max_gap,d_begin2end,"
            << "d_accumulate,max_jump,start_pen,start_day,start_time,end_pen,end_day,end_time\n";
    }
}

static bool write_tracks_and_append_summary(const fs::path& input_file,
    const std::vector<std::vector<Detection>>& tracks,
    const fs::path& per_id_tracks_csv,
    const fs::path& global_summary_csv)
{
    // 1) Write per-ID tracks file
    std::ofstream tracks_out(per_id_tracks_csv, std::ios::out);
    if (!tracks_out.is_open()) {
        std::cerr << "ERROR: Cannot write: " << per_id_tracks_csv << "\n";
        return false;
    }

    tracks_out << "custom_frame,ID,track_file_index,unique_track_id,pen,day,hour,minute,second,x,y,angle\n";

    std::ofstream sum_out(global_summary_csv, std::ios::app);
    if (!sum_out.is_open()) {
        std::cerr << "ERROR: Cannot append: " << global_summary_csv << "\n";
        return false;
    }

    const std::string base_name = input_file.stem().string(); // input file name without ".csv"
    const std::string per_id_file_name = per_id_tracks_csv.filename().string();

    for (size_t t = 0; t < tracks.size(); ++t) {
        const int track_file_index = static_cast<int>(t + 1);

        // unique_track_id = input filename minus .csv + "_" + track_file_index
        // Example: coord_paper4_ID4_1
        const std::string unique_track_id = base_name + "_" + std::to_string(track_file_index);

        const auto& tr = tracks[t];
        if (tr.empty()) continue;

        // ---- Write detection rows ----
        for (const auto& d : tr) {
            tracks_out
                << d.custom_frame << ","
                << d.ID << ","
                << track_file_index << ","
                << unique_track_id << ","
                << d.pen << ","
                << d.day << ","
                << d.timestamp.hour << ","
                << d.timestamp.minute << ","
                << d.timestamp.second << ","
                << d.cen_x << ","
                << d.cen_y << ","
                << d.angle << "\n";
        }

        // ---- Compute summary stats ----
        const long first_frame = tr.front().custom_frame;
        const long last_frame = tr.back().custom_frame;
        const long length = last_frame - first_frame;

        const auto& start_ts = tr.front().timestamp;
        const auto& end_ts = tr.back().timestamp;

        const int start_pen = tr.front().pen;
        const int start_day = tr.front().day;

        const int end_pen = tr.back().pen;
        const int end_day = tr.back().day;

        const std::string start_time = hhmmss(start_ts);
        const std::string end_time = hhmmss(end_ts);

        const long n_obs = static_cast<long>(tr.size());

        long   max_gap = 0;
        double d_begin2end = euclid(tr.front().cen_x, tr.front().cen_y,
            tr.back().cen_x, tr.back().cen_y);

        double d_accumulate = 0.0;
        double max_jump = 0.0;

        for (size_t i = 1; i < tr.size(); ++i) {
            const long gap = tr[i].custom_frame - tr[i - 1].custom_frame;
            if (gap > max_gap) max_gap = gap;

            const double step = euclid(tr[i - 1].cen_x, tr[i - 1].cen_y,
                tr[i].cen_x, tr[i].cen_y);
            d_accumulate += step;
            if (step > max_jump) max_jump = step;
        }

        // ---- Append summary row ----
        sum_out
            << unique_track_id << ","
            << per_id_file_name << ","
            << tr.front().ID << ","
            << first_frame << ","
            << last_frame << ","
            << length << ","
            << n_obs << ","
            << max_gap << ","
            << d_begin2end << ","
            << d_accumulate << ","
            << max_jump << ","
            << start_pen << ","
            << start_day << ","
            << start_time << ","
            << end_pen << ","
            << end_day << ","
            << end_time << "\n";
    }

    return true;
}

int get_tracks(const fs::path& input,
    const fs::path& output_dir,
    long frame_window,
    long min_len,
    double noise_dist,
    double max_speed,
    int local_window,
    double local_dist)
{
    int number_tracks = 0;

    const std::string input_stem = input.stem().string();

    // Global summary for the entire program
    const fs::path global_summary_csv = output_dir / (input_stem + "_track_summary.csv");

    // ------------------------------------------------------------
    // 0) Clean old intermediate/output files from previous runs
    // ------------------------------------------------------------
    std::error_code ec_clean;
    if (fs::exists(output_dir, ec_clean)) {
        for (const auto& entry : fs::directory_iterator(output_dir, ec_clean)) {
            if (ec_clean) {
                std::cerr << "!!! directory_iterator error during cleanup: "
                    << ec_clean.message() << "\n";
                break;
            }

            if (!entry.is_regular_file()) continue;

            const fs::path file = entry.path();
            if (file.extension() != ".csv") continue;

            const std::string stem = file.stem().string();

            const bool is_split_id_file =
                stem.rfind(input_stem + "_ID", 0) == 0 &&
                !ends_with(stem, "_tracks");

            const bool is_tracks_file =
                stem.rfind(input_stem + "_ID", 0) == 0 &&
                ends_with(stem, "_tracks");

            const bool is_summary_file =
                file.filename() == global_summary_csv.filename();

            if (is_split_id_file || is_tracks_file || is_summary_file) {
                std::error_code del_ec;
                fs::remove(file, del_ec);
                if (del_ec) {
                    std::cerr << "!!! Failed to remove old file " << file
                        << " : " << del_ec.message() << "\n";
                }
            }
        }
    }

    // ------------------------------------------------------------
    // 1) Read input once and group detections by ID
    // ------------------------------------------------------------
    std::vector<Detection> all_detections = ReadInput1(input);
    if (all_detections.empty()) {
        std::cerr << "No detections loaded from " << input << "\n";
        return 0;
    }

    std::unordered_map<int, std::vector<Detection>> detections_by_id;
    detections_by_id.reserve(128);

    for (auto& d : all_detections) {
        detections_by_id[d.ID].push_back(std::move(d));
    }

    all_detections.clear();
    all_detections.shrink_to_fit();

    std::vector<int> ids;
    ids.reserve(detections_by_id.size());
    for (const auto& kv : detections_by_id) {
        ids.push_back(kv.first);
    }
    std::sort(ids.begin(), ids.end());

    std::cout << "[INFO] Loaded detections for "
        << ids.size() << " IDs\n";

    // ------------------------------------------------------------
    // 2) Create fresh summary file
    // ------------------------------------------------------------
    std::ofstream(global_summary_csv, std::ios::out).close();
    ensure_summary_header_if_needed(global_summary_csv);

    // ------------------------------------------------------------
    // 3) Process each ID in memory
    // ------------------------------------------------------------
    for (int ID : ids)
    {
        auto det_it = detections_by_id.find(ID);
        if (det_it == detections_by_id.end()) continue;

        std::vector<Detection> detections = std::move(det_it->second);
        detections_by_id.erase(det_it);

        if (detections.empty()) continue;

        std::cout << "Processing ID " << ID
            << " (" << detections.size() << " detections)\n";

        std::sort(detections.begin(), detections.end(),
            [](const Detection& a, const Detection& b) {
                return a.custom_frame < b.custom_frame;
            });

        if (noise_dist > 0.0) {
            int removed_noise = 0;
            detections = filter_single_point_noise(
                detections, frame_window, noise_dist, removed_noise
            );

            if (removed_noise > 0) {
                std::cout << "  -> removed " << removed_noise
                    << " single-point noise detections"
                    << " using --noise_dist " << noise_dist << "\n";
            }
        }

        if (max_speed > 0.0) {
            int removed_speed_noise = 0;
            detections = filter_speed_noise(
                detections, frame_window, max_speed, removed_speed_noise
            );

            if (removed_speed_noise > 0) {
                std::cout << "  -> removed " << removed_speed_noise
                    << " speed-spike detections"
                    << " using --max_speed " << max_speed << "\n";
            }
        }

        if (local_dist > 0.0 && local_window >= 1) {
            int removed_local_noise = 0;
            detections = filter_local_trend_noise(
                detections, frame_window, local_window, local_dist, removed_local_noise
            );

            if (removed_local_noise > 0) {
                std::cout << "  -> removed " << removed_local_noise
                    << " local-trend noise detections"
                    << " using --local_window " << local_window
                    << " and --local_dist " << local_dist << "\n";
            }
        }

        auto tracks = build_tracks_per_id(detections, frame_window, min_len);
        if (tracks.empty()) {
            continue;
        }

        number_tracks += static_cast<int>(tracks.size());

        const fs::path synthetic_id_file =
            output_dir / (input_stem + "_ID" + std::to_string(ID) + ".csv");

        const fs::path per_id_tracks_csv =
            output_dir / (synthetic_id_file.stem().string() + "_tracks.csv");

        if (!write_tracks_and_append_summary(
            synthetic_id_file, tracks, per_id_tracks_csv, global_summary_csv
        )) {
            std::cerr << "!!! Failed writing outputs for ID: " << ID << "\n";
        }
        else {
            std::cout << "  -> wrote " << per_id_tracks_csv.filename()
                << " and appended to " << global_summary_csv.filename()
                << " (" << tracks.size() << " tracks)\n";
        }
    }

    std::cout << "[INFO] Total tracks written: " << number_tracks << "\n";

    return number_tracks;
}

static void interpolate_xy(long frame, const DetRow& a, const DetRow& b, double& x, double& y)
{
    // linear interpolation in time (frame index)
    const double t0 = static_cast<double>(a.frame);
    const double t1 = static_cast<double>(b.frame);
    const double t = static_cast<double>(frame);

    if (std::abs(t1 - t0) < 1e-12) {
        x = a.x;
        y = a.y;
        return;
    }
    const double w = (t - t0) / (t1 - t0);
    x = a.x + w * (b.x - a.x);
    y = a.y + w * (b.y - a.y);
}

static bool solve_quadratic_normal_equation(
    double s0,
    double s1,
    double s2,
    double s3,
    double s4,
    double v0,
    double v1,
    double v2,
    double& c0,
    double& c1,
    double& c2)
{
    double a[3][4] = {
        { s0, s1, s2, v0 },
        { s1, s2, s3, v1 },
        { s2, s3, s4, v2 }
    };

    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 3; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
                pivot = row;
            }
        }

        if (std::abs(a[pivot][col]) < 1e-12) return false;

        if (pivot != col) {
            for (int k = col; k < 4; ++k) {
                std::swap(a[col][k], a[pivot][k]);
            }
        }

        const double denom = a[col][col];
        for (int k = col; k < 4; ++k) {
            a[col][k] /= denom;
        }

        for (int row = 0; row < 3; ++row) {
            if (row == col) continue;

            const double factor = a[row][col];
            for (int k = col; k < 4; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    c0 = a[0][3];
    c1 = a[1][3];
    c2 = a[2][3];
    return true;
}

static bool quadratic_smooth_xy(
    long frame,
    const std::vector<DetRow>& dets,
    size_t cursor_index,
    const TrackInterval& interval,
    double& x,
    double& y,
    double& dir_x,
    double& dir_y)
{
    if (phenotype_smooth_window < 1 || dets.size() < 3) return false;

    const int max_points = 2 * phenotype_smooth_window + 1;
    std::vector<size_t> selected;
    selected.reserve(static_cast<size_t>(max_points));

    size_t right = cursor_index;
    while (right < dets.size() && dets[right].frame < frame) {
        ++right;
    }

    long left = static_cast<long>(right) - 1;

    while (static_cast<int>(selected.size()) < max_points &&
        (left >= 0 || right < dets.size())) {

        bool take_left = false;

        if (left < 0) {
            take_left = false;
        }
        else if (right >= dets.size()) {
            take_left = true;
        }
        else {
            const long left_diff = frame - dets[static_cast<size_t>(left)].frame;
            const long right_diff = dets[right].frame - frame;
            const long d_left = left_diff >= 0 ? left_diff : -left_diff;
            const long d_right = right_diff >= 0 ? right_diff : -right_diff;
            take_left = d_left <= d_right;
        }

        size_t idx = 0;
        if (take_left) {
            idx = static_cast<size_t>(left);
            --left;
        }
        else {
            idx = right;
            ++right;
        }

        if (dets[idx].frame < interval.first || dets[idx].frame > interval.last) {
            continue;
        }

        selected.push_back(idx);
    }

    if (selected.size() < 3) return false;

    std::sort(selected.begin(), selected.end(),
        [&](size_t lhs, size_t rhs) {
            return dets[lhs].frame < dets[rhs].frame;
        });

    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    double s3 = 0.0;
    double s4 = 0.0;

    double x0 = 0.0;
    double x1 = 0.0;
    double x2 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;

    for (size_t idx : selected) {
        const double u = static_cast<double>(dets[idx].frame - frame);
        const double u2 = u * u;
        const double u3 = u2 * u;
        const double u4 = u2 * u2;

        s0 += 1.0;
        s1 += u;
        s2 += u2;
        s3 += u3;
        s4 += u4;

        x0 += dets[idx].x;
        x1 += dets[idx].x * u;
        x2 += dets[idx].x * u2;

        y0 += dets[idx].y;
        y1 += dets[idx].y * u;
        y2 += dets[idx].y * u2;
    }

    double cx0 = 0.0;
    double cx1 = 0.0;
    double cx2 = 0.0;
    double cy0 = 0.0;
    double cy1 = 0.0;
    double cy2 = 0.0;

    if (!solve_quadratic_normal_equation(s0, s1, s2, s3, s4, x0, x1, x2, cx0, cx1, cx2)) {
        return false;
    }

    if (!solve_quadratic_normal_equation(s0, s1, s2, s3, s4, y0, y1, y2, cy0, cy1, cy2)) {
        return false;
    }

    x = cx0;
    y = cy0;

    const double dir_norm = std::sqrt(cx1 * cx1 + cy1 * cy1);
    if (dir_norm > 1e-12) {
        dir_x = cx1 / dir_norm;
        dir_y = cy1 / dir_norm;
    }

    return true;
}

static std::string time_to_string(const VideoTime& t)
{
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << t.hour << ":"
        << std::setw(2) << std::setfill('0') << t.minute << ":"
        << std::setw(2) << std::setfill('0') << t.second;
    return oss.str();
}

static std::vector<Event> build_events(const std::vector<TrackSummary>& tracks,
    std::vector<TrackInterval>& intervals_out)
{
    intervals_out.clear();
    intervals_out.reserve(tracks.size());

    std::vector<Event> ev;
    ev.reserve(tracks.size() * 2);

    for (const auto& t : tracks) {
        TrackInterval in{};
        in.ID = t.ID;
        in.first = t.first_frame;
        in.last = t.last_frame;
        in.unique_track_id = t.unique_track_id;
        in.track_file = t.track_file;

        const size_t idx = intervals_out.size();
        intervals_out.push_back(std::move(in));

        ev.push_back(Event{ intervals_out[idx].first, true,  intervals_out[idx].ID, idx });
        ev.push_back(Event{ intervals_out[idx].last + 1, false, intervals_out[idx].ID, idx });
    }

    std::sort(ev.begin(), ev.end(),
        [](const Event& a, const Event& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            // process ends before starts at same frame? here end frame is last+1 so usually not needed
            return a.is_start < b.is_start;
        });

    return ev;
}

static void reset_hourly_accumulators(
    std::vector<HourlyIndAccum>& ind_accum,
    std::vector<HourlyPairAccum>& pair_accum)
{
    for (auto& x : ind_accum) x = HourlyIndAccum{};
    for (auto& x : pair_accum) x = HourlyPairAccum{};
}

static void write_hourly_individual(
    std::ofstream& fout_ind,
    int pen,
    int day,
    int hour,
    const std::vector<int>& unique_ids,
    const std::vector<HourlyIndAccum>& ind_accum)
{
    for (int s = 0; s < static_cast<int>(unique_ids.size()); ++s)
    {
        const auto& acc = ind_accum[s];
        if (acc.n_frames == 0) continue;

        const double mean_speed =
            acc.n_speed_valid > 0
            ? acc.sum_speed / static_cast<double>(acc.n_speed_valid)
            : std::numeric_limits<double>::quiet_NaN();

        const double moving_prop =
            acc.n_speed_total > 0
            ? static_cast<double>(acc.n_moving) / static_cast<double>(acc.n_speed_total)
            : std::numeric_limits<double>::quiet_NaN();

        const double mean_density_r =
            acc.sum_density_r / static_cast<double>(acc.n_frames);

        const double mean_front_density =
            acc.n_moving > 0
            ? acc.sum_front_density / static_cast<double>(acc.n_moving)
            : std::numeric_limits<double>::quiet_NaN();

        const double mean_back_density =
            acc.n_moving > 0
            ? acc.sum_back_density / static_cast<double>(acc.n_moving)
            : std::numeric_limits<double>::quiet_NaN();

        const double mean_front_minus_back =
            acc.n_moving > 0
            ? acc.sum_front_minus_back / static_cast<double>(acc.n_moving)
            : std::numeric_limits<double>::quiet_NaN();

        fout_ind
            << pen << ","
            << day << ","
            << hour << ","
            << unique_ids[s] << ","
            << time_to_string(acc.start_ts) << ","
            << time_to_string(acc.end_ts) << ","
            << acc.n_frames << ","
            << mean_speed << ","
            << moving_prop << ","
            << mean_density_r << ","
            << mean_front_density << ","
            << mean_back_density << ","
            << mean_front_minus_back
            << "\n";
    }
}

struct DensityLookup {
    int radius_bins = 0;
    int side = 0;

    double step = 5.0;
    double reach = 0.0;
    double max_region_r = 0.0;
    double circle_self = 0.0;
    double front_self = 0.0;
    double back_self = 0.0;

    std::vector<double> circle;
    std::vector<double> front;
    std::vector<double> back;

    DensityLookup() = default;

    DensityLookup(double sigma,
        double circle_r,
        double sector_r,
        double sector_theta_deg,
        double step_)
    {
        step = step_ > 0.0 ? step_ : 5.0;

        max_region_r = std::max(circle_r, sector_r);
        reach = max_region_r + 3.0 * sigma;
        radius_bins = static_cast<int>(std::ceil(reach / step));
        side = 2 * radius_bins + 1;

        const size_t n = static_cast<size_t>(side) * static_cast<size_t>(side);
        circle.assign(n, 0.0);
        front.assign(n, 0.0);
        back.assign(n, 0.0);

        if (sigma <= 0.0) return;

        const double sigma2 = sigma * sigma;
        const double norm_const = 1.0 / (2.0 * kPI * sigma2);
        const double cutoff = 3.0 * sigma;
        const double cutoff2 = cutoff * cutoff;
        const double cell_area = step * step;

        const double circle_r2 = circle_r * circle_r;
        const double sector_r2 = sector_r * sector_r;
        const double theta_rad = sector_theta_deg * kPI / 180.0;
        const double cos_theta = std::cos(theta_rad);

        const int circle_cell_rad = static_cast<int>(std::ceil(circle_r / step));
        const int sector_cell_rad = static_cast<int>(std::ceil(sector_r / step));
        const int region_cell_rad = std::max(circle_cell_rad, sector_cell_rad);

        for (int sy = -radius_bins; sy <= radius_bins; ++sy) {
            for (int sx = -radius_bins; sx <= radius_bins; ++sx) {
                double circle_sum = 0.0;
                double front_sum = 0.0;
                double back_sum = 0.0;

                const double source_x = static_cast<double>(sx) * step;
                const double source_y = static_cast<double>(sy) * step;

                for (int py = -region_cell_rad; py <= region_cell_rad; ++py) {
                    const double y = static_cast<double>(py) * step;

                    for (int px = -region_cell_rad; px <= region_cell_rad; ++px) {
                        const double x = static_cast<double>(px) * step;
                        const double point_r2 = x * x + y * y;

                        const bool in_circle = point_r2 <= circle_r2;
                        bool in_front = false;
                        bool in_back = false;

                        double sector_weight = 0.0;
                        if (point_r2 <= sector_r2 && point_r2 > 1e-24) {
                            const double point_r = std::sqrt(point_r2);
                            const double ux = x / point_r;

                            in_front = ux >= cos_theta;
                            in_back = -ux >= cos_theta;

                            const double half_sector_r = 0.5 * sector_r;
                            if (point_r <= half_sector_r || sector_r <= 0.0) {
                                sector_weight = 1.0;
                            }
                            else {
                                sector_weight = (sector_r - point_r) / half_sector_r;
                                if (sector_weight < 0.0) sector_weight = 0.0;
                            }
                        }

                        if (!in_circle && !in_front && !in_back) continue;

                        const double dx = x - source_x;
                        const double dy = y - source_y;
                        const double d2 = dx * dx + dy * dy;
                        if (d2 > cutoff2) continue;

                        const double val =
                            norm_const * std::exp(-d2 / (2.0 * sigma2)) * cell_area;

                        if (in_circle) circle_sum += val;
                        if (in_front) front_sum += val * sector_weight;
                        if (in_back) back_sum += val * sector_weight;
                    }
                }

                const size_t idx = index(sx, sy);
                circle[idx] = circle_sum;
                front[idx] = front_sum;
                back[idx] = back_sum;
            }
        }

        const size_t self_idx = index(0, 0);
        circle_self = circle[self_idx];
        front_self = front[self_idx];
        back_self = back[self_idx];
    }

    size_t index(int sx, int sy) const
    {
        const int ix = sx + radius_bins;
        const int iy = sy + radius_bins;
        return static_cast<size_t>(iy) * static_cast<size_t>(side)
            + static_cast<size_t>(ix);
    }

    bool inside_bins(int sx, int sy) const
    {
        return sx >= -radius_bins && sx <= radius_bins
            && sy >= -radius_bins && sy <= radius_bins;
    }

    void add_contribution(double dx_local,
        double dy_local,
        double& circle_sum,
        double& front_sum,
        double& back_sum) const
    {
        const int sx = static_cast<int>(std::llround(dx_local / step));
        const int sy = static_cast<int>(std::llround(dy_local / step));

        if (!inside_bins(sx, sy)) return;

        const size_t idx = index(sx, sy);
        circle_sum += circle[idx];
        front_sum += front[idx];
        back_sum += back[idx];
    }
};

static double gaussian_density_at(
    double x,
    double y,
    const FrameObs& source)
{
    if (density_sigma <= 0.0) return 0.0;

    const double dx = x - source.x;
    const double dy = y - source.y;
    const double d2 = dx * dx + dy * dy;

    const double cutoff = 3.0 * density_sigma;
    if (d2 > cutoff * cutoff) return 0.0;

    const double sigma2 = density_sigma * density_sigma;
    const double norm_const = 1.0 / (2.0 * kPI * sigma2);
    return norm_const * std::exp(-d2 / (2.0 * sigma2));
}

static bool focal_region_crosses_density_bounds(
    const FrameObs& focal,
    const DensityLookup& lookup)
{
    const double r = lookup.max_region_r;

    return focal.x - r < density_min_x ||
        focal.x + r > density_max_x ||
        focal.y - r < density_min_y ||
        focal.y + r > density_max_y;
}

static void direct_bounded_density_for_focal(
    const std::vector<FrameObs>& rows,
    int focal_index,
    const std::vector<int>& neighbor_indices,
    double& circle_sum,
    double& front_sum,
    double& back_sum)
{
    circle_sum = 0.0;
    front_sum = 0.0;
    back_sum = 0.0;

    const FrameObs& focal = rows[focal_index];

    const double step = density_grid_step > 0.0 ? density_grid_step : 5.0;
    const double cell_area = step * step;

    const int nx = std::max(
        1,
        static_cast<int>(std::ceil((density_max_x - density_min_x) / step)) + 1
    );
    const int ny = std::max(
        1,
        static_cast<int>(std::ceil((density_max_y - density_min_y) / step)) + 1
    );

    auto x_to_ix = [&](double x) -> int {
        return static_cast<int>(std::llround((x - density_min_x) / step));
        };
    auto y_to_iy = [&](double y) -> int {
        return static_cast<int>(std::llround((y - density_min_y) / step));
        };
    auto ix_to_x = [&](int ix) -> double {
        return density_min_x + static_cast<double>(ix) * step;
        };
    auto iy_to_y = [&](int iy) -> double {
        return density_min_y + static_cast<double>(iy) * step;
        };

    double dir_x = focal.dir_x;
    double dir_y = focal.dir_y;
    const double dir_norm = std::sqrt(dir_x * dir_x + dir_y * dir_y);
    if (dir_norm < 1e-12) {
        dir_x = 1.0;
        dir_y = 0.0;
    }
    else {
        dir_x /= dir_norm;
        dir_y /= dir_norm;
    }

    const double circle_r2 = density_r * density_r;
    const double sector_r2 = density_sector_r * density_sector_r;
    const double theta_rad = density_sector_theta_deg * kPI / 180.0;
    const double cos_theta = std::cos(theta_rad);

    const double max_r = std::max(density_r, density_sector_r);
    const int ix0 = x_to_ix(focal.x);
    const int iy0 = y_to_iy(focal.y);
    const int rad = static_cast<int>(std::ceil(max_r / step));

    for (int iy = iy0 - rad; iy <= iy0 + rad; ++iy) {
        if (iy < 0 || iy >= ny) continue;

        const double y = iy_to_y(iy);
        const double dy = y - focal.y;

        for (int ix = ix0 - rad; ix <= ix0 + rad; ++ix) {
            if (ix < 0 || ix >= nx) continue;

            const double x = ix_to_x(ix);
            const double dx = x - focal.x;
            const double d2 = dx * dx + dy * dy;

            const bool in_circle = d2 <= circle_r2;
            bool in_front = false;
            bool in_back = false;
            double sector_weight = 0.0;

            if (d2 <= sector_r2 && d2 > 1e-24) {
                const double d = std::sqrt(d2);
                const double ux = dx / d;
                const double uy = dy / d;
                const double c_front = dir_x * ux + dir_y * uy;
                const double c_back = -dir_x * ux - dir_y * uy;

                in_front = c_front >= cos_theta;
                in_back = c_back >= cos_theta;

                const double half_sector_r = 0.5 * density_sector_r;
                if (d <= half_sector_r || density_sector_r <= 0.0) {
                    sector_weight = 1.0;
                }
                else {
                    sector_weight = (density_sector_r - d) / half_sector_r;
                    if (sector_weight < 0.0) sector_weight = 0.0;
                }
            }

            if (!in_circle && !in_front && !in_back) continue;

            double val = 0.0;
            for (int j : neighbor_indices) {
                if (density_exclude_self != 0 && j == focal_index) continue;
                val += gaussian_density_at(x, y, rows[j]);
            }

            const double mass = val * cell_area;
            if (in_circle) circle_sum += mass;
            if (in_front) front_sum += mass * sector_weight;
            if (in_back) back_sum += mass * sector_weight;
        }
    }
}

struct SpatialHash {
    double cell_size = 1.0;
    std::unordered_map<long long, std::vector<int>> cells;

    explicit SpatialHash(double cell_size_)
    {
        cell_size = cell_size_ > 0.0 ? cell_size_ : 1.0;
    }

    static long long key(int ix, int iy)
    {
        const unsigned long long ux = static_cast<unsigned int>(ix);
        const unsigned long long uy = static_cast<unsigned int>(iy);
        return static_cast<long long>((ux << 32) ^ uy);
    }

    int coord_to_cell(double x) const
    {
        return static_cast<int>(std::floor(x / cell_size));
    }

    void build(const std::vector<FrameObs>& rows)
    {
        cells.clear();
        cells.reserve(rows.size() * 2);

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const int ix = coord_to_cell(rows[i].x);
            const int iy = coord_to_cell(rows[i].y);
            cells[key(ix, iy)].push_back(i);
        }
    }
};

static void compute_traits_for_frame(
    const std::vector<FrameObs>& rows,
    long frame,
    std::vector<TraitsPerInd>& out_traits,
    const DensityLookup& lookup,
    SpatialHash& spatial_hash)
{
    const int n = static_cast<int>(rows.size());
    out_traits.assign(n, TraitsPerInd{});

    spatial_hash.build(rows);
    const auto& hash_cells = spatial_hash.cells;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < n; ++i) {
        out_traits[i].frame = frame;
        out_traits[i].ID = rows[i].ID;

        double dir_x = rows[i].dir_x;
        double dir_y = rows[i].dir_y;
        const double dir_norm = std::sqrt(dir_x * dir_x + dir_y * dir_y);

        if (dir_norm < 1e-12) {
            dir_x = 1.0;
            dir_y = 0.0;
        }
        else {
            dir_x /= dir_norm;
            dir_y /= dir_norm;
        }

        const double left_x = -dir_y;
        const double left_y = dir_x;

        double circle_sum = 0.0;
        double front_sum = 0.0;
        double back_sum = 0.0;

        const int cx = spatial_hash.coord_to_cell(rows[i].x);
        const int cy = spatial_hash.coord_to_cell(rows[i].y);
        const int cell_rad = static_cast<int>(std::ceil(lookup.reach / spatial_hash.cell_size));
        const double reach2 = lookup.reach * lookup.reach;

        std::vector<int> neighbor_indices;
        neighbor_indices.reserve(static_cast<size_t>(n));

        for (int gy = cy - cell_rad; gy <= cy + cell_rad; ++gy) {
            for (int gx = cx - cell_rad; gx <= cx + cell_rad; ++gx) {
                const auto it = hash_cells.find(SpatialHash::key(gx, gy));
                if (it == hash_cells.end()) continue;

                for (int j : it->second) {
                    const double dx = rows[j].x - rows[i].x;
                    const double dy = rows[j].y - rows[i].y;
                    if (dx * dx + dy * dy > reach2) continue;

                    neighbor_indices.push_back(j);
                }
            }
        }

        if (focal_region_crosses_density_bounds(rows[i], lookup)) {
            direct_bounded_density_for_focal(
                rows,
                i,
                neighbor_indices,
                circle_sum,
                front_sum,
                back_sum
            );
        }
        else {
            for (int j : neighbor_indices) {
                if (density_exclude_self != 0 && j == i) continue;

                const double dx = rows[j].x - rows[i].x;
                const double dy = rows[j].y - rows[i].y;

                if (dx * dx + dy * dy > reach2) continue;

                    const double dx_local = dx * dir_x + dy * dir_y;
                    const double dy_local = dx * left_x + dy * left_y;

                    lookup.add_contribution(
                        dx_local,
                        dy_local,
                        circle_sum,
                        front_sum,
                        back_sum
                    );
            }
        }

        out_traits[i].density_r = circle_sum;
        out_traits[i].front_density = front_sum;
        out_traits[i].back_density = back_sum;
        out_traits[i].front_minus_back = front_sum - back_sum;
    }
}

static void set_density_bounds_from_observations(
    const std::unordered_map<int, std::vector<DetRow>>& id_to_dets)
{
    bool has_point = false;
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;

    for (const auto& kv : id_to_dets) {
        for (const auto& d : kv.second) {
            if (!has_point) {
                min_x = max_x = d.x;
                min_y = max_y = d.y;
                has_point = true;
            }
            else {
                min_x = std::min(min_x, d.x);
                max_x = std::max(max_x, d.x);
                min_y = std::min(min_y, d.y);
                max_y = std::max(max_y, d.y);
            }
        }
    }

    if (!has_point) return;

    const double margin =
        std::max(density_r, density_sector_r) + 3.0 * density_sigma;

    density_min_x = min_x - margin;
    density_max_x = max_x + margin;
    density_min_y = min_y - margin;
    density_max_y = max_y + margin;
}

void calculate_phenotype(const fs::path& track_summary_csv, const fs::path& out_csv)
{
    std::vector<TrackSummary> track_info_list = load_tracks(track_summary_csv);

    if (track_info_list.empty()) {
        std::cerr << "No tracks loaded from " << track_summary_csv << "\n";
        return;
    }

    long min_start_frame = track_info_list[0].first_frame;
    long max_end_frame = track_info_list[0].last_frame;

    for (const auto& t : track_info_list) {
        min_start_frame = std::min(min_start_frame, t.first_frame);
        max_end_frame = std::max(max_end_frame, t.last_frame);
    }

    std::cout << "Min start frame: " << min_start_frame << "\n";
    std::cout << "Max end frame: " << max_end_frame << "\n";
    std::cout << "Using fps: " << fps << "\n";
    std::cout << "Active speed window: "
        << active_speed_window << " frames\n";
    std::cout << "Phenotype quadratic smoothing window: "
        << phenotype_smooth_window << "\n";

    std::cout << "Density parameters:\n";
    std::cout << "  sigma = " << density_sigma << "\n";
    std::cout << "  density_r = " << density_r << "\n";
    std::cout << "  sector_r = " << density_sector_r << "\n";
    std::cout << "  sector_theta_deg = " << density_sector_theta_deg << "\n";
    std::cout << "  grid_step = " << density_grid_step << "\n";
    std::cout << "  exclude_self = " << density_exclude_self << "\n";

    std::unordered_map<int, std::string> id_to_file;
    id_to_file.reserve(track_info_list.size());

    for (const auto& t : track_info_list) {
        auto it = id_to_file.find(t.ID);
        if (it == id_to_file.end()) {
            id_to_file.emplace(t.ID, t.track_file);
        }
        else if (it->second != t.track_file) {
            std::cerr << "Warning: inconsistent track_file for ID " << t.ID << "\n";
        }
    }

    std::vector<int> unique_ids;
    unique_ids.reserve(id_to_file.size());

    for (const auto& kv : id_to_file) {
        unique_ids.push_back(kv.first);
    }

    std::sort(unique_ids.begin(), unique_ids.end());

    std::unordered_map<int, int> id_to_slot;
    id_to_slot.reserve(unique_ids.size());

    for (int s = 0; s < static_cast<int>(unique_ids.size()); ++s) {
        id_to_slot[unique_ids[s]] = s;
    }

    const int N = static_cast<int>(unique_ids.size());

    std::unordered_map<int, std::vector<DetRow>> id_to_dets;
    id_to_dets.reserve(id_to_file.size());

    for (const auto& kv : id_to_file) {
        const int ID = kv.first;
        const fs::path file = track_summary_csv.parent_path() / kv.second;

        id_to_dets[ID] = load_id_detections(file);

        if (id_to_dets[ID].empty()) {
            std::cerr << "Warning: ID " << ID
                << " has 0 detections in " << file << "\n";
        }
    }

    set_density_bounds_from_observations(id_to_dets);

    std::cout << "Effective density bounds from observed coordinates:\n";
    std::cout << "  x range = [" << density_min_x << ", " << density_max_x << "]\n";
    std::cout << "  y range = [" << density_min_y << ", " << density_max_y << "]\n";

    std::vector<TrackInterval> intervals;
    std::vector<Event> events = build_events(track_info_list, intervals);

    std::unordered_map<int, size_t> active_track;
    active_track.reserve(id_to_file.size());

    std::unordered_map<int, size_t> cursor;
    cursor.reserve(id_to_file.size());

    std::ofstream fout_ind(out_csv);
    if (!fout_ind.is_open()) {
        std::cerr << "Error opening output: " << out_csv << "\n";
        return;
    }

    fout_ind
        << "pen,day,hour,ID,start_ts,end_ts,n_frames,"
        << "mean_speed,moving_prop,"
        << "density_r,front_density,back_density,front_minus_back\n";

    std::vector<FrameObs> frame_rows;
    std::vector<TraitsPerInd> traits;

    std::vector<HourlyIndAccum> ind_accum(N);

    std::vector<double> prev_x(N, 0.0);
    std::vector<double> prev_y(N, 0.0);
    std::vector<long> prev_frame(N, -1);
    std::vector<bool> has_prev_pos(N, false);
    std::vector<std::deque<FrameObs>> active_speed_history(N);

    DensityLookup density_lookup(
        density_sigma,
        density_r,
        density_sector_r,
        density_sector_theta_deg,
        density_grid_step
    );

    SpatialHash spatial_hash(density_lookup.reach);

    std::cout << "Density lookup bins: "
        << density_lookup.side << " x " << density_lookup.side
        << " = " << static_cast<long long>(density_lookup.side) * density_lookup.side
        << "\n";
    std::cout << "Density neighbor search radius: "
        << density_lookup.reach << "\n";

    bool hour_initialized = false;
    int current_pen = -1;
    int current_day = -1;
    int current_hour = -1;

    size_t epos = 0;

    for (long i = min_start_frame; i <= max_end_frame; ++i)
    {
        while (epos < events.size() && events[epos].frame == i) {
            const Event& ev = events[epos];

            if (ev.is_start) {
                active_track[ev.ID] = ev.interval_index;
                cursor[ev.ID] = 0;

                auto slot_it = id_to_slot.find(ev.ID);
                if (slot_it != id_to_slot.end()) {
                    const int s = slot_it->second;
                    has_prev_pos[s] = false;
                    prev_frame[s] = -1;
                    active_speed_history[s].clear();
                }
            }
            else {
                auto slot_it = id_to_slot.find(ev.ID);
                if (slot_it != id_to_slot.end()) {
                    const int s = slot_it->second;
                    has_prev_pos[s] = false;
                    prev_frame[s] = -1;
                    active_speed_history[s].clear();
                }

                active_track.erase(ev.ID);
                cursor.erase(ev.ID);
            }

            ++epos;
        }

        frame_rows.clear();
        frame_rows.reserve(active_track.size());

        for (const auto& kv : active_track)
        {
            const int ID = kv.first;
            const TrackInterval& interval = intervals[kv.second];

            if (i < interval.first || i > interval.last) continue;

            auto itD = id_to_dets.find(ID);
            if (itD == id_to_dets.end()) continue;

            const auto& dets = itD->second;
            if (dets.empty()) continue;

            size_t& k = cursor[ID];

            while (k + 1 < dets.size() && dets[k + 1].frame <= i) {
                ++k;
            }

            FrameObs fo{};
            fo.frame = i;
            fo.ID = ID;

            const DetRow& a = dets[k];

            if (a.frame == i) {
                fo.observed = true;
                fo.x = a.x;
                fo.y = a.y;
                fo.pen = a.pen;
                fo.day = a.day;
                fo.dir_x = std::cos(a.angle);
                fo.dir_y = std::sin(a.angle);
            }
            else {
                if (k + 1 >= dets.size()) continue;

                const DetRow& b = dets[k + 1];

                if (!(a.frame < i && i < b.frame)) continue;

                fo.observed = false;
                interpolate_xy(i, a, b, fo.x, fo.y);

                fo.pen = a.pen;
                fo.day = a.day;

                // keep previous direction for interpolated positions
                fo.dir_x = std::cos(a.angle);
                fo.dir_y = std::sin(a.angle);
            }

            quadratic_smooth_xy(
                i,
                dets,
                k,
                interval,
                fo.x,
                fo.y,
                fo.dir_x,
                fo.dir_y
            );

            frame_rows.push_back(fo);
        }

        if (frame_rows.empty()) continue;

        VideoTime frame_ts = frame_to_time(i, fps);

        for (auto& fo : frame_rows) {
            fo.ts = frame_ts;
        }

        const int frame_day = frame_rows[0].day;
        const int frame_hour = frame_rows[0].ts.hour;
        const int frame_pen = frame_rows[0].pen;

        if (!hour_initialized) {
            current_pen = frame_pen;
            current_day = frame_day;
            current_hour = frame_hour;
            hour_initialized = true;
        }
        else if (
            frame_pen != current_pen ||
            frame_day != current_day ||
            frame_hour != current_hour
            ) {
            write_hourly_individual(
                fout_ind,
                current_pen,
                current_day,
                current_hour,
                unique_ids,
                ind_accum
            );

            for (auto& x : ind_accum) {
                x = HourlyIndAccum{};
            }

            current_pen = frame_pen;
            current_day = frame_day;
            current_hour = frame_hour;
        }

        compute_traits_for_frame(
            frame_rows,
            i,
            traits,
            density_lookup,
            spatial_hash
        );

        for (size_t idx = 0; idx < frame_rows.size(); ++idx)
        {
            const FrameObs& fo = frame_rows[idx];
            const TraitsPerInd& tr = traits[idx];

            const int s = id_to_slot[fo.ID];
            auto& acc = ind_accum[s];

            acc.n_frames++;

            bool has_step_speed = false;
            double step_speed = 0.0;

            if (has_prev_pos[s] && prev_frame[s] < fo.frame) {
                const long gap = fo.frame - prev_frame[s];

                if (gap > 0) {
                    const double d = euclid(
                        prev_x[s],
                        prev_y[s],
                        fo.x,
                        fo.y
                    );

                    const double speed =
                        d * fps / static_cast<double>(gap);

                    step_speed = speed;
                    has_step_speed = true;
                }
            }

            bool has_active_speed = false;
            double active_speed = 0.0;

            if (has_step_speed && active_speed_window < 1) {
                active_speed = step_speed;
                has_active_speed = true;
            }
            else {
                auto& hist = active_speed_history[s];

                while (
                    hist.size() >= 2 &&
                    fo.frame - hist[1].frame >= active_speed_window
                    ) {
                    hist.pop_front();
                }

                if (!hist.empty() && hist.front().frame < fo.frame) {
                    const FrameObs& past = hist.front();
                    const long active_gap = fo.frame - past.frame;

                    if (active_gap > 0) {
                        const double active_dist = euclid(
                            past.x,
                            past.y,
                            fo.x,
                            fo.y
                        );

                        active_speed =
                            active_dist * fps / static_cast<double>(active_gap);
                        has_active_speed = true;
                    }
                }
            }

            if (has_step_speed && has_active_speed) {
                acc.n_speed_total++;

                if (active_speed > moving_speed_threshold) {
                    acc.sum_speed += step_speed;
                    acc.n_speed_valid++;
                    acc.n_moving++;
                    acc.sum_front_density += tr.front_density;
                    acc.sum_back_density += tr.back_density;
                    acc.sum_front_minus_back += tr.front_minus_back;
                }
            }

            prev_x[s] = fo.x;
            prev_y[s] = fo.y;
            prev_frame[s] = fo.frame;
            has_prev_pos[s] = true;
            active_speed_history[s].push_back(fo);

            acc.sum_density_r += tr.density_r;

            if (!acc.has_time) {
                acc.start_ts = fo.ts;
                acc.end_ts = fo.ts;
                acc.has_time = true;
            }
            else {
                if (time_to_seconds(fo.ts) < time_to_seconds(acc.start_ts)) {
                    acc.start_ts = fo.ts;
                }

                if (time_to_seconds(fo.ts) > time_to_seconds(acc.end_ts)) {
                    acc.end_ts = fo.ts;
                }
            }
        }
    }

    if (hour_initialized) {
        write_hourly_individual(
            fout_ind,
            current_pen,
            current_day,
            current_hour,
            unique_ids,
            ind_accum
        );
    }

    fout_ind.close();

    std::cout << "Finished.\n";
    std::cout << "Wrote:\n";
    std::cout << "  " << out_csv << "\n";
}

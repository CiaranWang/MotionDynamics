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
    long min_len)
{
    int number_tracks = 0;

    split_input_by_id(input, output_dir);

    // Global summary for the entire program
    const fs::path global_summary_csv = output_dir / (input.stem().string() + "_track_summary.csv");
    std::ofstream(global_summary_csv, std::ios::out).close();
    ensure_summary_header_if_needed(global_summary_csv);

    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(output_dir, ec))
    {
        if (ec) {
            std::cerr << "!!! directory_iterator error: " << ec.message() << "\n";
            break;
        }
        
        if (!entry.is_regular_file()) continue;

        const fs::path file = entry.path();
        if (file.extension() != ".csv") continue;
        if (file.filename() == global_summary_csv.filename()) continue;
        if (ends_with(file.stem().string(), "_tracks")) continue;

        std::cout << "Processing CSV: " << file << "\n";
        std::vector<Detection> detections = ReadInput_Tzayhri(file);
        if (detections.empty()) continue;

        auto tracks = build_tracks_per_id(detections, frame_window, min_len);
        if (tracks.empty()) continue;
        
        number_tracks += static_cast<int>(tracks.size());

        const fs::path per_id_tracks_csv = output_dir / (file.stem().string() + "_tracks.csv");

        if (!write_tracks_and_append_summary(file, tracks, per_id_tracks_csv, global_summary_csv)) {
            std::cerr << "!!! Failed writing outputs for: " << file << "\n";
        }
        else {
            std::cout << "  -> wrote " << per_id_tracks_csv.filename()
                << " and appended to " << global_summary_csv.filename()
                << " (" << tracks.size() << " tracks)\n";

            // DELETE processed file
            std::error_code del_ec;
            if (fs::remove(file, del_ec)) {
                std::cout << "  -> deleted source file: " << file.filename() << "\n";
            }
            else if (del_ec) {
                std::cerr << "!!! Failed to delete " << file
                    << " : " << del_ec.message() << "\n";
            }
        }
    }

    return number_tracks;
}
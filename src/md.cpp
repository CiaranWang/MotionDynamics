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

static void compute_traits_for_frame(
    const std::vector<FrameObs>& rows,
    long frame,
    std::vector<TraitsPerInd>& out_traits,
    std::vector<PairWithin>& out_pairs_r4
)
{
    const int n = static_cast<int>(rows.size());
    out_traits.assign(n, TraitsPerInd{});

    // init outputs
    for (int i = 0; i < n; ++i) {
        out_traits[i].frame = frame;
        out_traits[i].ID = rows[i].ID;
        out_traits[i].mean_dist_r2 = std::numeric_limits<double>::quiet_NaN();
    }

    std::vector<double> sum_dist_r2(n, 0.0);
    out_pairs_r4.clear();
    out_pairs_r4.reserve(static_cast<size_t>(n) * static_cast<size_t>(n - 1) / 2);

    // pairwise loop
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            // pen mates only
            if (rows[i].pen != rows[j].pen) continue;

            const double dx = rows[i].x - rows[j].x;
            const double dy = rows[i].y - rows[j].y;
            const double dist_pix = std::sqrt(dx * dx + dy * dy);
            const double dist = dist_pix * scale_factor;


            // 1) count within r1
            if (dist <= r1) {
                out_traits[i].n_within_r1++;
                out_traits[j].n_within_r1++;
            }

            // 2) Trait 2
            if (dist <= r2) {
                out_traits[i].n_within_r2++;
                out_traits[j].n_within_r2++;
                sum_dist_r2[i] += dist;
                sum_dist_r2[j] += dist;
            }

            // Trait 3 (sum over all within r3): 1/(dist + e3_pix)
            if (dist <= r3) {
                const double val = 1.0 / (dist + e3);
                out_traits[i].prox_intensity_r3 += val;
                out_traits[j].prox_intensity_r3 += val;
            }

            // Trait 4 
            PairWithin pw{};
            pw.frame = frame;
            pw.ts = rows[i].ts;

            pw.ID1 = rows[i].ID;
            pw.observed1 = rows[i].observed ? 1 : 0;

            pw.ID2 = rows[j].ID;
            pw.observed2 = rows[j].observed ? 1 : 0;

            pw.pen = rows[i].pen;
            pw.day = rows[i].day;

            pw.dist = dist;
            pw.within_r4 = (dist <= r4) ? 1 : 0;

            out_pairs_r4.push_back(pw);

            // Trait 5: weight within r5out
            if (dist <= r5out) {
                double w = 0.0;
                if (dist <= r5in) {
                    w = 1.0;
                }
                else {
                    w = (r5out - dist) / (r5out - r5in);
                }
                out_traits[i].personal_space_r5 += w;
                out_traits[j].personal_space_r5 += w;
            }
        }
    }

    // finalize trait 2
    for (int i = 0; i < n; ++i)
    {
        if (out_traits[i].n_within_r2 > 0) {
            out_traits[i].mean_dist_r2 = sum_dist_r2[i] / static_cast<double>(out_traits[i].n_within_r2);
        }
        else {
            out_traits[i].mean_dist_r2 = std::numeric_limits<double>::quiet_NaN(); // NaN
        }
    }
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

        const double trait1 = acc.sum_trait1 / static_cast<double>(acc.n_frames);
        const double trait3 = acc.sum_trait3 / static_cast<double>(acc.n_frames);
        const double trait5 = acc.sum_trait5 / static_cast<double>(acc.n_frames);

        double trait2 = std::numeric_limits<double>::quiet_NaN();
        if (acc.n_trait2_valid > 0) {
            trait2 = acc.sum_trait2 / static_cast<double>(acc.n_trait2_valid);
        }

        fout_ind
            << pen << ","
            << day << ","
            << hour << ","
            << unique_ids[s] << ","
            << time_to_string(acc.start_ts) << ","
            << time_to_string(acc.end_ts) << ","
            << acc.n_frames << ","
            << trait1 << ","
            << trait2 << ","
            << trait3 << ","
            << trait5
            << "\n";
    }
}

static void write_hourly_pairs(
    std::ofstream& fout_pair,
    int pen,
    int day,
    int hour,
    const std::vector<int>& unique_ids,
    const std::vector<HourlyPairAccum>& pair_accum)
{
    const int N = static_cast<int>(unique_ids.size());

    for (int s1 = 0; s1 < N; ++s1)
    {
        for (int s2 = s1 + 1; s2 < N; ++s2)
        {
            const auto& acc = pair_accum[s1 * N + s2];
            if (acc.n_frames == 0) continue;

            const double mean_dist = acc.sum_dist / static_cast<double>(acc.n_frames);
            const double prop_within_r4 = acc.sum_within_r4 / static_cast<double>(acc.n_frames);

            fout_pair
                << pen << ","
                << day << ","
                << hour << ","
                << unique_ids[s1] << ","
                << unique_ids[s2] << ","
                << time_to_string(acc.start_ts) << ","
                << time_to_string(acc.end_ts) << ","
                << acc.n_frames << ","
                << mean_dist << ","
                << prop_within_r4
                << "\n";
        }
    }
}


void calculate_phenotype(const fs::path& track_summary_csv, const fs::path& out_csv)
{
    // 1) Load track summary
    std::vector<TrackSummary> track_info_list = load_tracks(track_summary_csv);

    if (track_info_list.empty()) {
        std::cerr << "No tracks loaded from " << track_summary_csv << "\n";
        return;
    }

    // 2) Find global frame range
    long min_start_frame = track_info_list[0].first_frame;
    long max_end_frame = track_info_list[0].last_frame;

    for (const auto& t : track_info_list)
    {
        min_start_frame = std::min(min_start_frame, t.first_frame);
        max_end_frame = std::max(max_end_frame, t.last_frame);
    }

    std::cout << "Min start frame: " << min_start_frame << "\n";
    std::cout << "Max end frame: " << max_end_frame << "\n";

    // 3) Map ID -> detailed file (each ID has one file)
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

    // unique IDs + slot map
    std::vector<int> unique_ids;
    unique_ids.reserve(id_to_file.size());
    for (const auto& kv : id_to_file) unique_ids.push_back(kv.first);
    std::sort(unique_ids.begin(), unique_ids.end());

    std::unordered_map<int, int> id_to_slot;
    id_to_slot.reserve(unique_ids.size());
    for (int s = 0; s < static_cast<int>(unique_ids.size()); ++s) {
        id_to_slot[unique_ids[s]] = s;
    }

    const int N = static_cast<int>(unique_ids.size());


    // 4) load all detections for each ID
    std::unordered_map<int, std::vector<DetRow>> id_to_dets;
    id_to_dets.reserve(id_to_file.size());
    for (const auto& kv : id_to_file) {
        const int ID = kv.first;
        const fs::path& file = track_summary_csv.parent_path() / kv.second;
        id_to_dets[ID] = load_id_detections(file);
        if (id_to_dets[ID].empty()) {
            std::cerr << "Warning: ID " << ID << " has 0 detections in " << file << "\n";
        }
    }

    std::vector<GlobalTimePoint> global_time_points = build_global_time_points(id_to_dets);
    if (global_time_points.empty()) {
        std::cerr << "Error: no global time support points could be built.\n";
        return;
    }

    for (size_t z = 1; z < global_time_points.size(); ++z) {
        if (global_time_points[z].frame <= global_time_points[z - 1].frame) {
            std::cerr << "Error: global_time_points not strictly increasing by frame.\n";
            return;
        }
    }

    // 5) events + active intervals
    std::vector<TrackInterval> intervals;
    std::vector<Event> events = build_events(track_info_list, intervals);

    // 6) Active track per ID (NO overlap, so single interval index)
    std::unordered_map<int, size_t> active_track; // ID -> interval_index
    active_track.reserve(id_to_file.size());

    // Per-active-ID cursor: index "k" such that dets[k].frame <= i < dets[k+1].frame
    std::unordered_map<int, size_t> cursor;
    cursor.reserve(id_to_file.size());

    // 8) Open output files
    std::ofstream fout_ind(out_csv);
    if (!fout_ind.is_open()) {
        std::cerr << "Error opening output: " << out_csv << "\n";
        return;
    }

    std::filesystem::path out_pairs_csv = out_csv;
    out_pairs_csv.replace_filename(
        out_csv.stem().string() + "_pair" + out_csv.extension().string()
    );

    std::ofstream fout_pair(out_pairs_csv);
    if (!fout_pair.is_open()) {
        std::cerr << "Error opening output: " << out_pairs_csv << "\n";
        return;
    }

    // Headers
    fout_ind 
        << "pen,day,hour,ID,start_ts,end_ts,n_frames,trait1,trait2,trait3,trait5\n";

    fout_pair
        << "pen,day,hour,ID1,ID2,start_timestamp,end_timestamp,n_frames,mean_dist_cm,prop_within_r4\n";

    // 9) frame iteration
    std::vector<FrameObs> frame_rows;
    std::vector<TraitsPerInd> traits;
    std::vector<PairWithin> pairs_r4;

    // hourly accumulators
    std::vector<HourlyIndAccum> ind_accum(N);
    std::vector<HourlyPairAccum> pair_accum(N * N);

    bool hour_initialized = false;
    int current_pen = -1;
    int current_day = -1;
    int current_hour = -1;

    size_t epos = 0;

    size_t gidx = 0;
    GlobalTimePoint prev_tp{};
    GlobalTimePoint next_tp{};
    bool have_prev_tp = false;
    bool have_next_tp = false;

    if (!global_time_points.empty()) {
        next_tp = global_time_points[0];
        have_next_tp = true;
    }

    for (long i = min_start_frame; i <= max_end_frame; ++i)
    {
        // Apply all events at frame i
        // Update active track
        while (epos < events.size() && events[epos].frame == i) {
            const Event& ev = events[epos];

            if (ev.is_start) {
                active_track[ev.ID] = ev.interval_index;
                cursor[ev.ID] = 0;
            }
            else {
                active_track.erase(ev.ID);
                cursor.erase(ev.ID);
            }
            ++epos;
        }

        // Build per-frame list (observed + interpolated)
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
            while (k + 1 < dets.size() && dets[k + 1].frame <= i) ++k;

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
            }
            else {
                if (k + 1 >= dets.size()) continue;
                const DetRow& b = dets[k + 1];
                if (!(a.frame < i && i < b.frame)) continue;

                fo.observed = false;
                interpolate_xy(i, a, b, fo.x, fo.y);

                fo.pen = a.pen;
                fo.day = a.day;
            }
            frame_rows.push_back(fo);
        }

        if (frame_rows.empty()) continue;

        // Advance global support points so that:
        // prev_tp.frame < = i < = next_tp.frame (when available)
        while (gidx < global_time_points.size() && global_time_points[gidx].frame < i) {
            prev_tp = global_time_points[gidx];
            have_prev_tp = true;
            ++gidx;
        }

        if (gidx < global_time_points.size()) {
            next_tp = global_time_points[gidx];
            have_next_tp = true;
        }
        else {
            have_next_tp = false;
        }

        VideoTime frame_ts{};

        if (have_next_tp && next_tp.frame == i) {
            frame_ts = seconds_to_time(next_tp.sec);
        }
        else if (have_prev_tp && have_next_tp) {
            frame_ts = interpolate_between_points(
                i,
                prev_tp.frame, prev_tp.sec,
                next_tp.frame, next_tp.sec
            );
        }
        else if (have_prev_tp) {
            frame_ts = seconds_to_time(prev_tp.sec);
        }
        else if (have_next_tp) {
            frame_ts = seconds_to_time(next_tp.sec);
        }
        else {
            std::cerr << "Error: no global timestamp support available at frame " << i << "\n";
            continue;
        }

        // all IDs at the same frame get the same timestamp
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
        else if (frame_pen != current_pen || frame_day != current_day || frame_hour != current_hour) {
            write_hourly_individual(fout_ind, current_pen, current_day, current_hour, unique_ids, ind_accum);
            write_hourly_pairs(fout_pair, current_pen, current_day, current_hour, unique_ids, pair_accum);

            reset_hourly_accumulators(ind_accum, pair_accum);

            current_pen = frame_pen;
            current_day = frame_day;
            current_hour = frame_hour;
        }

        // Compute frame level traits 1,2,3,5 per ID + trait 4 per pair
        compute_traits_for_frame(frame_rows, i, traits, pairs_r4);

        // accumulate individual
        for (size_t idx = 0; idx < frame_rows.size(); ++idx)
        {
            const FrameObs& fo = frame_rows[idx];
            const TraitsPerInd& tr = traits[idx];

            const int s = id_to_slot[fo.ID];
            auto& acc = ind_accum[s];

            acc.n_frames++;
            acc.sum_trait1 += tr.n_within_r1;
            acc.sum_trait3 += tr.prox_intensity_r3;
            acc.sum_trait5 += tr.personal_space_r5;

            if (!std::isnan(tr.mean_dist_r2)) {
                acc.sum_trait2 += tr.mean_dist_r2;
                acc.n_trait2_valid++;
            }

            if (!acc.has_time) {
                acc.start_ts = fo.ts;
                acc.end_ts = fo.ts;
                acc.has_time = true;
            }
            else {
                if (time_to_seconds(fo.ts) < time_to_seconds(acc.start_ts))
                    acc.start_ts = fo.ts;
                if (time_to_seconds(fo.ts) > time_to_seconds(acc.end_ts))
                    acc.end_ts = fo.ts;
            }
        }

        // accumulate pairs
        for (const auto& pw : pairs_r4)
        {
            int id1 = pw.ID1;
            int id2 = pw.ID2;
            if (id1 > id2) std::swap(id1, id2);

            const int s1 = id_to_slot[id1];
            const int s2 = id_to_slot[id2];

            auto& acc = pair_accum[s1 * N + s2];
            acc.n_frames++;
            acc.sum_dist += pw.dist;
            acc.sum_within_r4 += pw.within_r4;

            if (!acc.has_time) {
                acc.start_ts = pw.ts;
                acc.end_ts = pw.ts;
                acc.has_time = true;
            }
            else {
                if (time_to_seconds(pw.ts) < time_to_seconds(acc.start_ts))
                    acc.start_ts = pw.ts;
                if (time_to_seconds(pw.ts) > time_to_seconds(acc.end_ts))
                    acc.end_ts = pw.ts;
            }
        }
    }

    // flush last hour
    if (hour_initialized) {
        write_hourly_individual(fout_ind, current_pen, current_day, current_hour, unique_ids, ind_accum);
        write_hourly_pairs(fout_pair, current_pen, current_day, current_hour, unique_ids, pair_accum);
    }

    fout_ind.close();
    fout_pair.close();

    std::cout << "Finished.\n";
    std::cout << "Wrote:\n";
    std::cout << "  " << out_csv << "\n";
    std::cout << "  " << out_pairs_csv << "\n";
}
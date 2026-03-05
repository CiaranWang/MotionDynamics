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
static VideoTime time_at_frame(long frame, long anchor_frame, const VideoTime& anchor_time, double fps)
{
    const double dt = static_cast<double>(frame - anchor_frame) / fps;
    const long dsec = static_cast<long>(std::llround(dt)); // nearest second
    return seconds_to_time(time_to_seconds(anchor_time) + dsec);
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

static bool find_exact_by_frame(const std::vector<DetRow>& dets, long frame, DetRow& out)
{
    auto it = std::lower_bound(dets.begin(), dets.end(), frame,
        [](const DetRow& d, long f) { return d.frame < f; });
    if (it != dets.end() && it->frame == frame) {
        out = *it;
        return true;
    }
    return false;
}

static bool find_prev_next_in_bounds(
    const std::vector<DetRow>& dets,
    long frame,
    long lo, long hi,
    DetRow& prev, DetRow& next)
{
    // first element with d.frame >= frame
    auto it = std::lower_bound(dets.begin(), dets.end(), frame,
        [](const DetRow& d, long f) { return d.frame < f; });

    // next: at it (or later) but must be within bounds and > frame
    auto it_next = it;
    while (it_next != dets.end() && it_next->frame < lo) ++it_next;
    while (it_next != dets.end() && it_next->frame <= frame) ++it_next;
    if (it_next == dets.end() || it_next->frame > hi) return false;

    // prev: element before it, but within bounds and < frame
    auto it_prev = it;
    if (it_prev == dets.begin()) return false;
    --it_prev;
    while (true) {
        if (it_prev->frame < lo) return false;
        if (it_prev->frame < frame) break;
        if (it_prev == dets.begin()) return false;
        --it_prev;
    }

    prev = *it_prev;
    next = *it_next;
    return true;
}

static void interpolate_xy(long frame, const DetRow& a, const DetRow& b, double& x, double& y)
{
    // linear interpolation in time (frame index)
    const double t0 = static_cast<double>(a.frame);
    const double t1 = static_cast<double>(b.frame);
    const double t = static_cast<double>(frame);

    if (std::abs(t1 - t0) < 1e-12) {
        x = a.x; y = a.y;
        return;
    }
    const double w = (t - t0) / (t1 - t0);
    x = a.x + w * (b.x - a.x);
    y = a.y + w * (b.y - a.y);
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
                double w;
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
        id_to_file[t.ID] = t.track_file;
    }

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
    // 5) events + active intervals
    std::vector<TrackInterval> intervals;
    std::vector<Event> events = build_events(track_info_list, intervals);

    // 6) Active track per ID (NO overlap, so single interval index)
    std::unordered_map<int, size_t> active_track; // ID -> interval_index
    active_track.reserve(id_to_file.size());

    // Per-active-ID cursor: index "k" such that dets[k].frame <= i < dets[k+1].frame
    std::unordered_map<int, size_t> cursor;
    cursor.reserve(id_to_file.size());

    // 7) Anchor for fps-based timestamp
    bool have_anchor = false;
    long anchor_frame = 0;
    VideoTime anchor_time{};

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
        << "frame,ID,observed,pen,day,hh,mm,ss,x_pix,y_pix,"
        << "trait1,"
        << "trait2,"
        << "trait3,"
        << "trait5\n";

    fout_pair
        << "frame,timestap,ID1,observed1,ID2,observed2,pen,day,dist_cm,within_r4\n";

    // 9) frame iteration
    std::vector<FrameObs> frame_rows;
    std::vector<TraitsPerInd> traits;
    std::vector<PairWithin> pairs_r4;

    size_t epos = 0;

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
                // observed
                fo.observed = true;
                fo.x = a.x; fo.y = a.y;
                fo.pen = a.pen; fo.day = a.day;
                fo.ts = a.ts;

                if (!have_anchor) {
                    have_anchor = true;
                    anchor_frame = i;
                    anchor_time = a.ts;
                }
            }
            else {
                // interpolated
                if (k + 1 >= dets.size()) continue;
                const DetRow& b = dets[k + 1];
                if (!(a.frame < i && i < b.frame)) continue;

                fo.observed = false;
                interpolate_xy(i, a, b, fo.x, fo.y);

                fo.pen = a.pen;
                fo.day = a.day;

                if (have_anchor) fo.ts = time_at_frame(i, anchor_frame, anchor_time, fps);
                else {
                    fo.ts = a.ts; // fallback (early frames)
                    cerr << "Warning, did not find anchor to interpolate timestamp \n";            
                }
            }

            frame_rows.push_back(fo);
        }

        if (frame_rows.empty()) continue;

        // Compute traits 1,2,3,5 per ID + trait 4 per pair
        compute_traits_for_frame(frame_rows, i, traits, pairs_r4);

        // Write output rows
        for (size_t idx = 0; idx < frame_rows.size(); ++idx)
        {
            const FrameObs& fo = frame_rows[idx];
            const TraitsPerInd& tr = traits[idx];

            fout_ind
                << fo.frame << ","
                << fo.ID << ","
                << (fo.observed ? 1 : 0) << ","
                << fo.pen << ","
                << fo.day << ","
                << fo.ts.hour << ","
                << fo.ts.minute << ","
                << fo.ts.second << ","
                << fo.x << ","
                << fo.y << ","
                << tr.n_within_r1 << ","
                << tr.mean_dist_r2 << ","
                << tr.prox_intensity_r3 << ","
                << tr.personal_space_r5
                << "\n";
        }

        // Write PAIR file (all pairs among IDs present)
        for (const auto& pw : pairs_r4)
        {
            fout_pair
                << pw.frame << ","
                << pw.ts.hour << ":" << pw.ts.minute << ":" << pw.ts.second << ","
                << pw.ID1 << ","
                << pw.observed1 << ","
                << pw.ID2 << ","
                << pw.observed2 << ","
                << pw.pen << ","
                << pw.day << ","
                << pw.dist << ","
                << pw.within_r4
                << "\n";
        }
    }

    fout_ind.close();
    fout_pair.close();

    std::cout << "Finished.\nWrote:\n  " << out_csv << "\n  " << out_pairs_csv << "\n";
}
#include "ReadInput.h"
#include "md.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <list>
#include <stdexcept>
#include <filesystem>  // C++17
#include <iomanip>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

double scale_factor = 1.0;
double scale_factor_inv = 1.0;

double fps = 30.0;

double moving_speed_threshold = 5.0;

double density_sigma = 50.0;
double density_r = 100.0;
double density_sector_r = 100.0;
double density_sector_theta_deg = 30.0;

double density_grid_step = 5.0;
double density_min_x = 0.0;
double density_max_x = 0.0;
double density_min_y = 0.0;
double density_max_y = 0.0;

int density_exclude_self = 1;

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> splitCSV(const std::string& line) {
    std::vector<std::string> out;
    out.reserve(40); // your header has ~33 cols

    std::string cur;
    cur.reserve(64);

    bool in_quotes = false;

    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
        }
        else if (c == ',' && !in_quotes) {
            out.push_back(cur);
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static int parse_third_column_id(const std::string& line)
{
    // We want the 3rd column: time_MS, frame, ID, ...
    // Find 1st, 2nd, 3rd commas; ID is between 2nd and 3rd.
    size_t c1 = line.find(',');
    if (c1 == std::string::npos) throw std::runtime_error("Malformed line (no comma #1).");

    size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) throw std::runtime_error("Malformed line (no comma #2).");

    size_t c3 = line.find(',', c2 + 1);
    if (c3 == std::string::npos) throw std::runtime_error("Malformed line (no comma #3).");

    // Extract ID substring
    const std::string id_str = line.substr(c2 + 1, c3 - (c2 + 1));
    // stoi tolerates leading/trailing spaces, but we expect clean CSV
    return std::stoi(id_str);
}

static int parse_id_from_column(const std::string& line, size_t id_col)
{
    auto cols = splitCSV(line);
    if (id_col >= cols.size()) {
        throw std::runtime_error("Malformed line (missing id column).");
    }

    return std::stoi(trim(cols[id_col]));
}

static bool parse_int(const std::string& s, int& v) {
    try { v = static_cast<int>(std::stod(s)); return true; }
    catch (...) { return false; }
}
static bool parse_long(const std::string& s, long& v) {
    try { v = static_cast<long>(std::stod(s)); return true; }
    catch (...) { return false; }
}
static bool parse_double(const std::string& s, double& v) {
    try { v = std::stod(s); return true; }
    catch (...) { return false; }
}

static bool parse_hhmmss(const std::string& s, VideoTime& t)
{
    // expects "HH:MM:SS" (len 8). Also tolerates whitespace.
    auto isdigit2 = [](char c) { return c >= '0' && c <= '9'; };

    std::string x = s;
    // trim (minimal)
    size_t a = x.find_first_not_of(" \t\r\n");
    size_t b = x.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    x = x.substr(a, b - a + 1);

    if (x.size() != 8 || x[2] != ':' || x[5] != ':') return false;
    if (!isdigit2(x[0]) || !isdigit2(x[1]) ||
        !isdigit2(x[3]) || !isdigit2(x[4]) ||
        !isdigit2(x[6]) || !isdigit2(x[7])) return false;

    t.hour = (x[0] - '0') * 10 + (x[1] - '0');
    t.minute = (x[3] - '0') * 10 + (x[4] - '0');
    t.second = (x[6] - '0') * 10 + (x[7] - '0');

    if (t.hour < 0 || t.hour > 23) return false;
    if (t.minute < 0 || t.minute > 59) return false;
    if (t.second < 0 || t.second > 59) return false;

    return true;
}

std::string hhmmss(const VideoTime& t)
{
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << t.hour << ":"
        << std::setw(2) << std::setfill('0') << t.minute << ":"
        << std::setw(2) << std::setfill('0') << t.second;
    return oss.str();
}

void split_input_by_id(const fs::path input_file, const fs::path output_dir)
{
    std::ifstream in(input_file); 
    if (!in.is_open())
        throw std::runtime_error("Failed to open input file: " + input_file.string());

    // Read header (1st line)
    std::string header;
    if (!std::getline(in, header))
        throw std::runtime_error("Input file is empty: " + input_file.string());
    const std::string header_line = header + "\n";

    auto header_cols = splitCSV(header);
    size_t id_col = header_cols.size();
    for (size_t i = 0; i < header_cols.size(); ++i) {
        if (trim(header_cols[i]) == "id") {
            id_col = i;
            break;
        }
    }
    if (id_col == header_cols.size()) {
        throw std::runtime_error("Missing required column: id");
    }

    // LRU cache for output streams (avoid too many open files)
    constexpr size_t MAX_OPEN_FILES = 64;

    struct StreamEntry {
        std::ofstream ofs;
        std::list<int>::iterator lru_it;
        fs::path path;
    };

    std::unordered_map<int, StreamEntry> streams;
    std::list<int> lru; // front = most recently used, back = least

    auto make_output_path = [&](int id) -> fs::path {
        // e.g. input: signals.csv -> signals_ID3.csv
        return output_dir / (input_file.stem().string() + "_ID" + std::to_string(id) + input_file.extension().string());
    };

    auto touch_lru = [&](int id) {
        auto& entry = streams.at(id);
        lru.erase(entry.lru_it);
        lru.push_front(id);
        entry.lru_it = lru.begin();
    };

    auto close_one_if_needed = [&]() {
        if (streams.size() < MAX_OPEN_FILES) return;

        // Close least recently used (back)
        int victim_id = lru.back();
        lru.pop_back();

        auto it = streams.find(victim_id);
        if (it != streams.end()) {
            if (it->second.ofs.is_open()) it->second.ofs.close();
            streams.erase(it);
        }
    };

    auto ensure_header_written_for_file = [&](const fs::path& p, std::ofstream& ofs) {
        // If file doesn't exist or is empty, write header.
        // We check emptiness AFTER opening (append) to avoid TOCTOU issues.
        // file_size requires existence; use exists+file_size.
        bool need_header = true;
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            auto sz = fs::file_size(p, ec);
            if (!ec && sz > 0) need_header = false;
        }
        if (need_header) ofs << header_line;
    };

    auto get_stream_for_id = [&](int id) -> std::ofstream& {
        auto it = streams.find(id);
        if (it != streams.end()) {
            touch_lru(id);
            return it->second.ofs;
        }

        close_one_if_needed();

        StreamEntry entry;
        entry.path = make_output_path(id);
        entry.ofs.open(entry.path, std::ios::out | std::ios::app);
        if (!entry.ofs.is_open())
            throw std::runtime_error("Failed to open output file: " + entry.path.string());

        // Write header only if file is new/empty.
        ensure_header_written_for_file(entry.path, entry.ofs);

        // Add to cache + LRU
        lru.push_front(id);
        entry.lru_it = lru.begin();

        auto [inserted_it, ok] = streams.emplace(id, std::move(entry));
        (void)ok;
        return inserted_it->second.ofs;
    };

    // Stream through rows
    std::string line;
    size_t line_no = 1; // header is line 1
    size_t rows_written = 0;
    size_t rows_skipped = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;

        int id = 0;
        try {
            id = parse_id_from_column(line, id_col);
        }
        catch (const std::exception& e) {
            std::cerr << "Warning: skipping malformed line " << line_no << ": " << e.what() << "\n";
            ++rows_skipped;
            continue;
        }

        std::ofstream& out = get_stream_for_id(id);
        out << line << "\n";
        ++rows_written;
    }

    // Close all
    for (auto& kv : streams) {
        if (kv.second.ofs.is_open()) kv.second.ofs.close();
    }

    std::cerr << "Done. Wrote " << rows_written << " rows"
        << " (skipped " << rows_skipped << ") into: " << output_dir.string() << "\n";
}

static int parse_day_from_filename(const std::filesystem::path& input_file)
{
    const std::string name = input_file.filename().string();

    // 匹配开头8位数字：YYYYMMDD
    static const std::regex re(R"(^(\d{8}))");

    std::smatch m;
    if (std::regex_search(name, m, re)) {
        return std::stoi(m[1].str());
    }

    throw std::runtime_error("Cannot parse date from filename: " + name);
}

// Parser for coord_paper4.csv
std::vector<Detection> ReadInput1(const fs::path& input_file)
{
    std::vector<Detection> out;

    std::ifstream fin(input_file);

    if (!fin.is_open()) {
        std::cerr << "Error opening: " << input_file << "\n";
        return out;
    }

    std::string line;
    if (!std::getline(fin, line)) {
        std::cerr << "Empty file: " << input_file << "\n";
        return out;
    }

    auto header = splitCSV(line);
    std::unordered_map<std::string, size_t> col_idx;
    for (size_t i = 0; i < header.size(); ++i) {
        col_idx[trim(header[i])] = i;
    }

    auto get_col = [&](const std::string& name) -> size_t {
        auto it = col_idx.find(name);
        if (it == col_idx.end()) {
            throw std::runtime_error("Missing required column: " + name);
        }
        return it->second;
    };

    auto get_col_any = [&](const std::vector<std::string>& names) -> size_t {
        for (const auto& name : names) {
            auto it = col_idx.find(name);
            if (it != col_idx.end()) return it->second;
        }

        std::ostringstream oss;
        oss << "Missing required column. Tried: ";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) oss << ", ";
            oss << names[i];
        }
        throw std::runtime_error(oss.str());
        };

    const size_t idx_id = get_col("id");
    const size_t idx_custom_frame = get_col("custom_frame");
    const size_t idx_center_x = get_col_any({ "global_center_x", "center_x" });
    const size_t idx_center_y = get_col_any({ "global_center_y", "center_y" });
    const size_t idx_dir_x = get_col_any({ "global_dir_x", "dir_x" });
    const size_t idx_dir_y = get_col_any({ "global_dir_y", "dir_y" });

    // Optional columns
    //const bool has_scale_l = has_col("scale_l");
    //const bool has_n_markers = has_col("n_markers");
    //const bool has_conflict = has_col("boundary_conflict");

    while (std::getline(fin, line))
    {
        if (line.empty()) continue;

        auto cols = splitCSV(line);

        try {
            Detection d{};

            if (cols.size() <= std::max({ idx_id, idx_custom_frame, idx_center_x, idx_center_y, idx_dir_x, idx_dir_y })) {
                continue;
            }

            d.ID = std::stoi(cols[idx_id]);
            d.custom_frame = static_cast<long>(std::stoll(cols[idx_custom_frame]));
            d.cen_x = std::stod(cols[idx_center_x]);
            d.cen_y = std::stod(cols[idx_center_y]);
            d.dir_x = std::stod(cols[idx_dir_x]);
            d.dir_y = std::stod(cols[idx_dir_y]);

            d.pen = 0;
            d.day = parse_day_from_filename(input_file);   // 推荐
            d.timestamp.hour = 0;
            d.timestamp.minute = 0;
            d.timestamp.second = 0;

            // Optional: recompute norm just in case
            double norm = std::sqrt(d.dir_x * d.dir_x + d.dir_y * d.dir_y);
            if (norm > 1e-9) {
                d.dir_x /= norm;
                d.dir_y /= norm;
            }
            else {
                d.dir_x = 0.0;
                d.dir_y = 0.0;
            }

            d.angle = std::atan2(d.dir_y, d.dir_x);
            if (d.angle < 0) d.angle += 2.0 * kPI;

            out.push_back(d);
        }
        catch (...) {
            continue;
        }
    }

    std::cout << "Parsed " << out.size() << " detections from " << input_file << "\n";
    return out;
}

// Parser for coord_paper4.csv
std::vector<Detection> ReadInput_Tzayhri(const fs::path& input_file)
{
    std::vector<Detection> out;

    std::ifstream fin(input_file);

    if (!fin.is_open()) {
        std::cerr << "Error opening: " << input_file << "\n";
        return out;
    }

    std::string line;
    std::getline(fin, line);  // skip header

    while (std::getline(fin, line))
    {
        if (line.empty()) continue;

        auto cols = splitCSV(line);
        if (cols.size() <= 30) continue;

        try {
            Detection d{};
            d.ID = std::stoi(cols[2]);
            d.custom_frame = static_cast<long>(std::stod(cols[18]));
            d.cen_x = std::stod(cols[28]);
            d.cen_y = std::stod(cols[29]);

            d.pen = std::stoi(cols[20]);
            d.day = std::stoi(cols[22]);

            parse_hhmmss(cols[23], d.timestamp);

            double tlx = std::stod(cols[3]);
            double tly = std::stod(cols[4]);
            double trx = std::stod(cols[5]);
            double try_ = std::stod(cols[6]);
            double brx = std::stod(cols[7]);
            double bry = std::stod(cols[8]);
            double blx = std::stod(cols[9]);
            double bly = std::stod(cols[10]);

            // direction vector (top edge midpoint - bottom edge midpoint)
            double temp_dir_x = (tlx + trx) - (brx + blx);
            double temp_dir_y = (tly + try_) - (bry + bly);

            double norm = std::sqrt(temp_dir_x * temp_dir_x + temp_dir_y * temp_dir_y);
            if (norm > 1e-9) {
                d.dir_x = temp_dir_x / norm;
                d.dir_y = temp_dir_y / norm;
            }
            else {
                d.dir_x = 0.0;
                d.dir_y = 0.0;
            }

            d.angle = std::atan2(d.dir_y, d.dir_x);
            if (d.angle < 0) d.angle += 2.0 * kPI;

            out.push_back(d);
        }
        catch (...) {
            continue;
        }
    }

    std::cout << "Parsed " << out.size() << " detections from " << input_file << "\n";
    return out;
}

// ----------------------------
// Simplified .ini parser
// ----------------------------
bool load_parameters(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open " << filename << "\n";
        return false;
    }

    std::string line, section;
    while (std::getline(file, line)) {
        // remove comments
        if (auto pos = line.find('#'); pos != std::string::npos)
            line = line.substr(0, pos);

        // trim
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty()) continue;

        // section
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        // key = value
        if (auto pos = line.find('='); pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            double dval = std::stod(val);
            int ival = static_cast<int>(dval);

            // map key to global
            if (section == "geometry") {
                if (key == "scale_factor") scale_factor = dval;
                else if (key == "fps") fps = dval;
            }
            else if (section == "movement") {
                if (key == "speed_threshold") moving_speed_threshold = dval;
            }
            else if (section == "density") {
                if (key == "sigma") density_sigma = dval;
                else if (key == "r") density_r = dval;
                else if (key == "sector_r") density_sector_r = dval;
                else if (key == "sector_theta_deg") density_sector_theta_deg = dval;
                else if (key == "grid_step") density_grid_step = dval;
                else if (key == "exclude_self") density_exclude_self = ival;
            }
        }
    }

    // derived quantities
    scale_factor_inv = 1.0 / scale_factor;

    return true;
}

void print_parameters()
{
    std::cout << "===== Loaded Parameters =====\n";

    std::cout << "[geometry]\n";
    std::cout << "fps               = " << fps << "\n";
    std::cout << "scale_factor      = " << scale_factor << "\n\n";

    std::cout << "[movement]\n";
    std::cout << "speed_threshold   = " << moving_speed_threshold << "\n\n";

    std::cout << "[density]\n";
    std::cout << "sigma             = " << density_sigma << "\n";
    std::cout << "r                 = " << density_r << "\n";
    std::cout << "sector_r          = " << density_sector_r << "\n";
    std::cout << "sector_theta_deg  = " << density_sector_theta_deg << "\n";
    std::cout << "grid_step         = " << density_grid_step << "\n";
    std::cout << "exclude_self      = " << density_exclude_self << "\n";

    std::cout << "=============================\n\n";

    std::cout << "===== Derived Parameters =====\n";

    std::cout << "scale_factor_inv  = " << scale_factor_inv << "\n";
    std::cout << "=============================\n\n";

}

std::vector<TrackSummary> load_tracks(const fs::path& filename)
{
    std::vector<TrackSummary> tracks;

    std::ifstream fin(filename);
    if (!fin.is_open()) {
        std::cerr << "Error opening: " << filename << "\n";
        return tracks;
    }

    std::string line;
    std::getline(fin, line); // skip header

    while (std::getline(fin, line))
    {
        if (line.empty()) continue;

        auto cols = splitCSV(line);
        if (cols.size() < 17) continue;

        try
        {
            TrackSummary t{};

            t.unique_track_id = cols[0];
            t.track_file = cols[1];

            t.ID = std::stoi(cols[2]);

            // stod handles "5e+05"
            t.first_frame = static_cast<long>(std::stod(cols[3]));
            t.last_frame = static_cast<long>(std::stod(cols[4]));
            t.length = static_cast<long>(std::stod(cols[5]));

            t.n_obs = std::stoi(cols[6]);
            t.max_gap = static_cast<long>(std::stod(cols[7]));

            t.d_begin2end = std::stod(cols[8]);
            t.d_accumulate = std::stod(cols[9]);
            t.max_jump = std::stod(cols[10]);

            t.start_pen = std::stoi(cols[11]);
            t.start_day = std::stoi(cols[12]);

            parse_hhmmss(cols[13], t.start_time);

            t.end_pen = std::stoi(cols[14]);
            t.end_day = std::stoi(cols[15]);

            parse_hhmmss(cols[16], t.end_time);

            tracks.push_back(t);
        }
        catch (...)
        {
            continue;
        }
    }

    std::cout << "Parsed " << tracks.size()
        << " tracks from " << filename << "\n";

    return tracks;
}

std::vector<DetRow> load_id_detections(const fs::path& detailed_file)
{
    std::vector<DetRow> out;
    std::ifstream fin(detailed_file);
    if (!fin.is_open()) {
        std::cerr << "Error opening detailed file: " << detailed_file << "\n";
        return out;
    }

    std::string line;
    std::getline(fin, line); // header

    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        auto cols = splitCSV(line);
        if (cols.size() < 12) continue;

        try {
            DetRow d{};
            d.frame = static_cast<long>(std::stod(cols[0])); // custom_frame
            d.ID = std::stoi(cols[1]);

            d.pen = std::stoi(cols[4]);
            d.day = std::stoi(cols[5]);

            d.ts.hour = std::stoi(cols[6]);
            d.ts.minute = std::stoi(cols[7]);
            d.ts.second = std::stoi(cols[8]);

            d.x = std::stod(cols[9]);
            d.y = std::stod(cols[10]);
            d.angle = std::stod(cols[11]);

            out.push_back(d);
        }
        catch (...) {
            continue;
        }
    }

    std::sort(out.begin(), out.end(),
        [](const DetRow& a, const DetRow& b) { return a.frame < b.frame; });

    std::cout << "Loaded " << out.size() << " detections from " << detailed_file << "\n";
    return out;
}

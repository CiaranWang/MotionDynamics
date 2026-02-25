#include "ReadInput.h"
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

namespace fs = std::filesystem;

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
            id = parse_third_column_id(line);
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

// Parser for coord_paper4.csv
std::vector<Detection> ReadInput_Tzayhri(const fs::path& input_file) {
    std::vector<Detection> out;

    std::ifstream fin(input_file);

    if (!fin.is_open()) {
        std::cerr << "❌ Error opening: " << input_file << "\n";
        return out;
    }

    std::string line;
    std::getline(fin, line);  // skip header

    while (std::getline(fin, line)) {
        if (line.empty()) continue;

        auto cols = splitCSV(line);
        if (cols.size() <= 30) continue;

        try {
            Detection d{};
            d.ID = std::stoi(cols[2]);
            d.custom_frame = std::stol(cols[18]);
            d.cen_x = std::stod(cols[28]);
            d.cen_y = std::stod(cols[29]);

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
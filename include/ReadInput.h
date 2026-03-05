#ifndef READINPUT_H
#define READINPUT_H

#include <vector>
#include <string>
#include <filesystem>  // C++17
#include <iomanip>
#include <sstream>
#include "md.h"

extern double scale_factor;
extern double scale_factor_inv;
extern double fps;
extern double r1;
extern double r2;
extern double r3;
extern double e3;
extern double r4;
extern double r5in;
extern double r5out;

std::vector<Detection> ReadInput_Tzayhri(const std::filesystem::path& input_file);
std::string hhmmss(const VideoTime& t);
void split_input_by_id(const std::filesystem::path input_file, const std::filesystem::path output_dir);
bool load_parameters(const std::string& filename);
void print_parameters();

std::vector<TrackSummary> load_tracks(const std::filesystem::path& filename);
std::vector<DetRow> load_id_detections(const std::filesystem::path& detailed_file);
#endif
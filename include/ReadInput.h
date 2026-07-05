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

extern double moving_speed_threshold;
extern int active_speed_window;
extern int phenotype_smooth_window;

extern double density_sigma;
extern double density_r;
extern double density_sector_r;
extern double density_sector_theta_deg;

extern double density_grid_step;
extern double density_min_x;
extern double density_max_x;
extern double density_min_y;
extern double density_max_y;

extern int density_exclude_self;

std::vector<Detection> ReadInput_Tzayhri(const std::filesystem::path& input_file);
std::vector<Detection> ReadInput1(const std::filesystem::path& input_file);
std::string hhmmss(const VideoTime& t);
void split_input_by_id(const std::filesystem::path input_file, const std::filesystem::path output_dir);
bool load_parameters(const std::string& filename);
void print_parameters();

std::vector<TrackSummary> load_tracks(const std::filesystem::path& filename);
std::vector<DetRow> load_id_detections(const std::filesystem::path& detailed_file);
#endif

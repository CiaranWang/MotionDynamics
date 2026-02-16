#ifndef READINPUT_H
#define READINPUT_H

#include <vector>
#include <string>
#include <filesystem>  // C++17
#include "md.h"

std::vector<Detection> ReadInput_Tzayhri(const std::filesystem::path& input_file);
void split_input_by_id(const std::filesystem::path input_file, const std::filesystem::path output_dir);

#endif
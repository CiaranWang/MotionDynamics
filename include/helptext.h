#pragma once

static const char* HELP_TEXT = R"(Usage: ./MotionDynamic_4TO OPTIONS (Linux)
	Or MotionDynamic_4TO.exe OPTIONS (Windows)

Segement tracks from ArUco signals.

Options:
  -h, -H, --help          Show this help message and exit
  -u, -U, --update        Automatically update from github and rebuild 
  -v, -V, --version       Print program version and exit
  
  --track   This tells the program to use track mode
  -i /PATH/TO/INPUT_FILE.csv    Input file
  -o [/PATH/TO/OUTPUT_FOLDER]   Output folder
  --window [N]                Max allowed frame gaps within a track.(default: 200)
  --min_len [N]                Minium length of track.(default: 0)
  --noise_dist [D]             Remove one-frame position spikes when the previous and next
                               detections are within D pixels. 0 disables this. (default: 0)
  --max_speed [S]              Remove isolated points that require impossible speed jumps.
                               Unit: pixels per frame. 0 disables this. (default: 0)
  --local_window [N]           Number of neighboring detections searched on each side for
                               local trend filtering. (default: 2)
  --local_dist [D]             Remove points farther than D pixels from the local interpolated
                               trend. 0 disables this. (default: 0)

  --cal_pheno         This tells the program to calculate phenotype
  -i /PATH/TO/INPUT_FILE.csv This is the track_summary file you get from --track mode. 
						and the .csv track file per ID should be in the same folder
  -p /PATH/TO/PARAMETER_FILE.ini

Linux Example:
  ./MotionDynamics_4TO --track -i 20231219_fitted_merge.csv
  ./MotionDynamics_4TO --track -i 20231219_fitted_merge.csv -o ./exp1 --window 250  --min_len 1000 --noise_dist 50 --max_speed 20 --local_dist 50
  ./MotionDynamics_4TO --cal_pheno -i 20231219_fitted_merge_track_summary.csv -p parameters.ini -o 20231219_fitted_merge_traits.csv
 
Windows Example:
  MotionDynamics_4TO.exe --track -i 20231219_fitted_merge.csv
  MotionDynamics_4TO.exe --track -i 20231219_fitted_merge.csv -o ./exp1 --window 250  --min_len 1000 --noise_dist 50 --max_speed 20 --local_dist 50
  MotionDynamics_4TO.exe --cal_pheno -i 20231219_fitted_merge_track_summary.csv -p parameters.ini -o 20231219_fitted_merge_traits.csv

Report bugs to: zhuoshi.wang@wur.nl
)";

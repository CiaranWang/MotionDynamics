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

  --cal_pheno         This tells the program to calculate phenotype (unfinished)


Linux Example:
  ./MotionDynamic_4TO --track -i coord_paper4.csv
  ./MotionDynamic_4TO --track -i coord_paper4.csv -o ./exp1 --window 250  --min_len 1000
 
Windows Example:
  MotionDynamic_4TO.exe --track -i coord_paper4.csv
  MotionDynamic_4TO.exe --track -i coord_paper4.csv -o ./exp1 --window 250  --min_len 1000

Report bugs to: zhuoshi.wang@wur.nl
)";
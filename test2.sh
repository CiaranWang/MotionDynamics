#!/bin/bash
#SBATCH -p main
#SBATCH -c 1
#SBATCH --mem=10G
#SBATCH -t 10:00:00
#SBATCH -J test_md
#SBATCH -o output_test_md.txt
#SBATCH -e error_test_md.txt
#SBATCH --array=1
#SBATCH --mail-type=ALL
#SBATCH --mail-user=zhuoshi.wang@wur.nl

cd ~/motion_dynamics_test/

##~/MotionDynamics_4TO/build/MotionDynamics_4TO --track -i ~/motion_dynamics_test/coord_paper4.csv -o ./test1 --min_len 1200
~/MotionDynamics_4TO/build/MotionDynamics_4TO --cal_pheno -i ./test1/coord_paper4_track_summary.csv -p ../MotionDynamics_4TO/parameters.ini -o ./test1/coord_paper4_traits.csv 

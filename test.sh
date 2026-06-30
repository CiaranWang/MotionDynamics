#!/bin/bash
#SBATCH -p main
#SBATCH -c 1
#SBATCH --mem=4G
#SBATCH -t 10:00:00
#SBATCH -J test_md
#SBATCH -o output_test_md.txt
#SBATCH -e error_test_md.txt
#SBATCH --array=1
#SBATCH --mail-type=ALL
#SBATCH --mail-user=zhuoshi.wang@wur.nl

cd ~/md_test_to/MotionDynamics/
mkdir -p test1
cd ./test1

~/md_test_to/MotionDynamics/build/MotionDynamics -i ~/motion_dynamics_test/coord_paper4.csv -o results1_ --frame_window 200 [--smooth]

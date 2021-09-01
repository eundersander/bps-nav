#!/bin/bash
set -e

export OMP_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=${OMP_NUM_THREADS}
export MKL_NUM_THREADS=${OMP_NUM_THREADS}

RUN_NAME="rgb"

set -x
python bps_nav/run.py \
    --exp-config bps_nav/config/pointnav/ddppo_pointnav.yaml \
    --run-type train \
    TASK_CONFIG.DATASET.SPLIT "train"

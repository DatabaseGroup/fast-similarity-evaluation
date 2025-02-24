#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/root/dschmitt/datasets/sets"
ARGS=()

generate_args() {
    local dataset=$1
    local threshold=$2

    ARGS+=("../twol -f ${DATASET_DIR}/${dataset} -e ${threshold} -m 0 -a allalloc -r 42 -i multi_reassessment --label ${LABEL_PREFIX}twol")
}

for threshold in 0.75 0.8 0.85 0.9 0.95; do
    generate_args bms-pos-dedup-raw.txt "${threshold}"
    generate_args dblpv14 "${threshold}"
    generate_args kosarak-dedup-raw.txt "${threshold}"
    generate_args lnonis1 "${threshold}"
done

export BINARY
parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

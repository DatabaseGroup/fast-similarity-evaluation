#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/home/fast-bench/datasets/strings/jw"
ARGS=()

generate_args() {
    local dataset=$1
    local threshold=$2

    ARGS+=("../fast_stats  -f ${DATASET_DIR}/${dataset} -d string -s jaro -t ${threshold} -l ${LABEL_PREFIX}jaro-ts -m time-static -x partition -i 0.2")
    ARGS+=("../limes-light -f ${DATASET_DIR}/${dataset} -t ${threshold} -l ${LABEL_PREFIX}jaro-limeslight")
}

for threshold in 0.85 0.88 0.91 0.94 0.97; do
    generate_args company_names "${threshold}"
    generate_args dblp "${threshold}"
    generate_args enron "${threshold}"
    generate_args trec "${threshold}"
    generate_args word "${threshold}"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

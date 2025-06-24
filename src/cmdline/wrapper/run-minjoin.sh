#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/home/fast-bench/datasets/strings"
ARGS=()

generate_args() {
    local dataset=$1
    local threshold=$2

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -t ${threshold} -d string -s sed -m time-static -i 0.2 -l ${LABEL_PREFIX}minjoin-ts-hq -a q10 q12 q14 -x partition")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -t ${threshold} -d string -s sed -m time-static -i 0.2 -l ${LABEL_PREFIX}minjoin-ts-mq -a q4 q6 q8 -x partition")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -t ${threshold} -d string -s sed -m time-static -i 0.2 -l ${LABEL_PREFIX}minjoin-ts-lq -a q1 q2 -x partition")
    ARGS+=("../minjoin    -f ${DATASET_DIR}/${dataset} -t ${threshold} -l ${LABEL_PREFIX}minjoin")
}

# our datasets
for threshold in {1..6}; do
    generate_args "minjoin/dblp" "$threshold"
    generate_args "minjoin/word" "$threshold"
done

for threshold in 2 4 6 8 10 12; do
    generate_args "minjoin/enron" "$threshold"
    generate_args "minjoin/trec" "$threshold"
done

# their datasets
for threshold in 5 10 15 20 25; do
    generate_args "minjoin/uniref" "${threshold}"
done

for threshold in 10 20 30 40 50; do
    generate_args "minjoin/minjtrec" "${threshold}"
done

for threshold in 50 75 100 125 150; do
    generate_args "minjoin/gen50ks" "${threshold}"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

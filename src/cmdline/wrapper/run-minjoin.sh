#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/root/dschmitt/datasets/strings/minjoin"
ARGS=()

generate_args() {
    local dataset=$1
    local threshold=$2

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -t ${threshold} -d string -s sed -m time-static -i 0.2 -l ${LABEL_PREFIX}minjoin-ts -x partition -y 3gram -a q6 q9 q12 q15")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -t ${threshold} -d string -s sed -m time-static -i 0.2 -l ${LABEL_PREFIX}minjoin-ts-withpartition -y 3gram -a q6 q9 q12 q15")
    ARGS+=("../minjoin    -f ${DATASET_DIR}/${dataset} -t ${threshold} -l ${LABEL_PREFIX}minjoin")
}

for threshold in 5 10 15 20 25; do
    generate_args uniref "${threshold}"
done

for threshold in 10 20 30 40 50; do
    generate_args trec "${threshold}"
done

for threshold in 50 75 100 125 150; do
    generate_args gen50ks "${threshold}"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

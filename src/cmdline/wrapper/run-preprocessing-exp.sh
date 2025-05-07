#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/home/fast-bench/datasets"
ARGS=()

generate_args_unprocessed() {
    local dataset=$1
    local datatype=$2
    local similarity=$3
    local threshold=$4

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-otf-prefix -m time-static -i 0.2 -x palloc partition")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-otf-palloc -m time-static -i 0.2 -x prefix-signature partition")
    ARGS+=("../set-preprocess -i ${DATASET_DIR}/${dataset} -o /dev/null -a")
}

generate_args_processed() {
    local dataset=$1
    local datatype=$2
    local similarity=$3
    local threshold=$4

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-pre-prefix -m time-static -i 0.2 -x palloc partition -e")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-pre-palloc -m time-static -i 0.2 -x prefix-signature partition -e")
}

for threshold in 0.75 0.80 0.85 0.90 0.95; do
    generate_args_processed "sets/shuf/kosarak-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_processed "sets/shuf/bms-pos-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_processed "sets/shuf/dblpv14" "set" "jaccard" "$threshold"
    generate_args_processed "sets/shuf/lnonis1" "set" "jaccard" "$threshold"
done

for threshold in 0.75 0.80 0.85 0.90 0.95; do
    generate_args_processed "sets/kosarak-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_processed "sets/bms-pos-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_processed "sets/dblpv14" "set" "jaccard" "$threshold"
    generate_args_processed "sets/lnonis1" "set" "jaccard" "$threshold"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

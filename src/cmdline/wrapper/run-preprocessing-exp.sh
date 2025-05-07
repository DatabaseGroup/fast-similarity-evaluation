#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/home/fast-bench/datasets"
ARGS=()

generate_args_sets() {
    local dataset=$1
    local datatype=$2
    local similarity=$3
    local threshold=$4

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-pre-prefix -m time-static -i 0.2 -x palloc partition -e")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-pre-palloc -m time-static -i 0.2 -x prefix-signature partition -e")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-otf-prefix -m time-static -i 0.2 -x palloc partition")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-otf-palloc -m time-static -i 0.2 -x prefix-signature partition")
    ARGS+=("../set-preprocess -i ${DATASET_DIR}/${dataset} -o /dev/null -a")
}

generate_args_strings() {
    local dataset=$1
    local datatype=$2
    local similarity=$3
    local threshold=$4

    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-pre-passjoin -m time-static -i 0.2 -x palloc partition prefix-signature -e")
    ARGS+=("../fast_stats -f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}pre-otf-passjoin -m time-static -i 0.2 -x palloc partition prefix-signature")
}

for threshold in 0.75 0.80 0.85 0.90 0.95; do
    generate_args_sets "sets/kosarak-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_sets "sets/bms-pos-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args_sets "sets/dblpv14" "set" "jaccard" "$threshold"
    generate_args_sets "sets/lnonis1" "set" "jaccard" "$threshold"
done

for threshold in {1..6}; do
    generate_args_strings "strings/dblp-presorted" "string" "sed" "$threshold"
    generate_args_strings "strings/word-presorted" "string" "sed" "$threshold"
done

for threshold in 2 4 6 8 10 12; do
    generate_args_strings "strings/enron-presorted" "string" "sed" "$threshold"
    generate_args_strings "strings/trec-presorted" "string" "sed" "$threshold"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

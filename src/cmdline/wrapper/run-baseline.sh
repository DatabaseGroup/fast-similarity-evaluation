#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py ../fast_stats"
DATASET_DIR="/root/dschmitt/datasets"
ARGS=()

generate_args() {
    local dataset=$1
    local datatype=$2
    local similarity=$3
    local threshold=$4

    case $datatype in
        set)
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-prefix -m time-static -i 0.2 -x palloc partition")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-palloc -m time-static -i 0.2 -x prefix-signature partition")
            ;;
        string)
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-qgram-prefix -m time-static -i 0.2 -x palloc pass-join partition")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-qgram-palloc -m time-static -i 0.2 -x prefix-signature pass-join partition")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-passjoin -m time-static -i 0.2 -x palloc prefix-signature partition")
            ;;
        tree)
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-tjoin -m time-static -i 0.2 -x palloc pass-join prefix-signature partition")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-labelset-palloc -m time-static -i 0.2 -x prefix-signature pass-join tjoin partition -y traversal-strings")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-labelset-prefix -m time-static -i 0.2 -x palloc pass-join tjoin partition -y traversal-strings")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-traversal-prefix -m time-static -i 0.2 -x palloc pass-join tjoin partition -y label-sets")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-traversal-palloc -m time-static -i 0.2 -x prefix-signature pass-join tjoin partition -y label-sets")
            ARGS+=("-f ${DATASET_DIR}/${dataset} -d ${datatype} -s ${similarity} -t ${threshold} -l ${LABEL_PREFIX}baseline-traversal-passjoin -m time-static -i 0.2 -x palloc prefix-signature tjoin partition -y label-sets")
            ;;
    esac
}

# For set datatype with jaccard similarity

for threshold in 0.75 0.80 0.85 0.90 0.95; do
    generate_args "sets/shuf/kosarak-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args "sets/shuf/bms-pos-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args "sets/shuf/livejournal-userswithgroups-raw.txt" "set" "jaccard" "$threshold"
    generate_args "sets/shuf/orkut-userswithgroups-dedup-raw.txt" "set" "jaccard" "$threshold"
    generate_args "sets/shuf/dblpv14" "set" "jaccard" "$threshold"
    generate_args "sets/shuf/lnonis1" "set" "jaccard" "$threshold"
done

# For string datatype with sed similarity
for threshold in {1..6}; do
    generate_args "strings/shuf/dblp" "string" "sed" "$threshold"
    generate_args "strings/shuf/word" "string" "sed" "$threshold"
done

for threshold in 2 4 6 8 10 12; do
    generate_args "strings/shuf/enron" "string" "sed" "$threshold"
    generate_args "strings/shuf/trec" "string" "sed" "$threshold"
done

# For tree datatype with ted similarity
for threshold in 2 4 6 8 10 12 14 16; do
    generate_args "trees/shuf/sentiment" "tree" "ted" "$threshold"
done

for threshold in 1 2 3 4 5 6; do
    generate_args "trees/shuf/synthetic" "tree" "ted" "$threshold"
done

for threshold in 1 2 3 4 5 6 7 8; do
    generate_args "trees/shuf/dblp" "tree" "ted" "$threshold"
done

for threshold in 4 8 12 16 20; do
    generate_args "trees/shuf/python" "tree" "ted" "$threshold"
done

for threshold in 5 10 15 20 25 30 35 40 45 50; do
    generate_args "trees/shuf/swissprot" "tree" "ted" "$threshold"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"
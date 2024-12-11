#!/bin/bash

source global.sh

BINARY="./venv/bin/python3 run.py"
DATASET_DIR="/root/dschmitt/datasets/trees/syncsig"
ARGS=()

generate_args() {
    local dataset=$1
    local threshold=$2

    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-static -i 0.2 -l ${LABEL_PREFIX}ts -x partition")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-static -i 0.2 -l ${LABEL_PREFIX}ts-withpartition")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-dynamic -i 0.2 -l ${LABEL_PREFIX}td-withpartition")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-dynamic -i 0.2 -l ${LABEL_PREFIX}tdw-withpartition -w")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-static -i 0.2 -l ${LABEL_PREFIX}ts-withoutcostly -x tjoin                         -y traversal-strings")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-static -i 0.2 -l ${LABEL_PREFIX}ts-lightweight   -x tjoin palloc                  -y traversal-strings")
    ARGS+=("../fast_stats    -f ${DATASET_DIR}/${dataset} -t ${threshold} -d tree -s ted -m time-static -i 0.2 -l ${LABEL_PREFIX}ts-partitiononly -x tjoin palloc prefix-signature -y traversal-strings")
    ARGS+=("../syncsignature -f ${DATASET_DIR}/${dataset} -t ${threshold} -a ejoin -l ${LABEL_PREFIX}syncsig-ejoin")
    ARGS+=("../syncsignature -f ${DATASET_DIR}/${dataset} -t ${threshold} -a bjoin -l ${LABEL_PREFIX}syncsig-bjoin")
}

for threshold in 10 15 20 25 30 35 40; do
    generate_args jscript1k "${threshold}"
    generate_args python1k "${threshold}"
    generate_args swissprot1k "${threshold}"
done

parallel -S "${REMOTE_SERVERS}" \
  --controlmaster \
  --progress \
  --workdir "${WORKING_DIR}" \
  -j "${JOBS}" \
  --shuf \
  --colsep ' ' \
  "$BINARY {}" ::: "${ARGS[@]}"

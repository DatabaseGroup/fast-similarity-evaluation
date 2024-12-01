import config
import queries

import csv
import os

import pymongo.collection
from pymongo.mongo_client import MongoClient
from pymongo.server_api import ServerApi

PREFIX = 'final-v9-'

def remove_extension(filename):
    return os.path.splitext(filename)[0]


def write_to_csv(filepath: str, headers: list[str], keys: list[str], data: dict[str, dict[str, any]]):
    with open(filepath, mode='w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(headers)

        for threshold in sorted(data.keys()):
            row = [threshold]
            for key in keys:
                row.append(data[threshold].get(key, ''))
            writer.writerow(row)


def static_vs_dynamic(datasets: list[str], similarity: str, collection: pymongo.collection.Collection):
    time_static_label = PREFIX + 'ts'
    time_dynamic_label = PREFIX + 'td'
    time_dynamic_warmup = PREFIX + 'tdw'
    labels = [time_static_label, time_dynamic_label, time_dynamic_warmup]

    for dataset in datasets:
        data = {}
        for label in labels:
            results = queries.average_time(collection, label, dataset, similarity)
            for result in results:
                threshold = result['_id']['threshold']
                avg_join_time = result['average_join_time']
                avg_build_time = result.get('average_build_time', 0)

                if threshold not in data:
                    data[threshold] = {}

                data[threshold][label] = avg_join_time
                if label == time_static_label:
                    data[threshold]['time-static-with-build'] = avg_join_time + avg_build_time

        filename = f'time/{remove_extension(dataset)}-{similarity}.csv'
        headers = ['Threshold', 'time-static', 'time-dynamic', 'time-dynamic-warmup', 'time-static-with-build']
        keys = [time_static_label, time_dynamic_label, time_dynamic_warmup, 'time-static-with-build']
        write_to_csv(filename, headers, keys, data)


def static_vs_baseline(datasets: list[str], similarity: str, collection: pymongo.collection.Collection):
    time_static_label = PREFIX + 'ts'
    set_baselines = ['baseline-prefix', 'baseline-palloc']
    string_baselines = ['baseline-qgram-prefix', 'baseline-qgram-palloc', 'baseline-passjoin']
    tree_baselines = ['baseline-tjoin', 'baseline-labelset-palloc', 'baseline-labelset-prefix',
                      'baseline-traversal-prefix', 'baseline-traversal-palloc', 'baseline-traversal-passjoin']

    for dataset in datasets:
        data = {}
        results = list(queries.average_time(collection, time_static_label, dataset, similarity))
        similarity = results[0]['_id']['similarity']
        baseline_labels = set_baselines if similarity in ['jaccard'] else string_baselines if similarity in [
            'sed'] else tree_baselines
        all_results = []  # type: list[pymongo.collection.Mapping]
        all_results.extend(results)
        for label in baseline_labels:
            all_results.extend(queries.average_time(collection, label, dataset, similarity))

        for result in all_results:
            threshold = result['_id']['threshold']
            avg_join_time = result['average_join_time']

            if threshold not in data:
                data[threshold] = {}

            data[threshold][result['_id']['label']] = avg_join_time

        filename = f'baseline/{remove_extension(dataset)}.csv'
        headers = ['Threshold', 'time-static']
        headers.extend([s.removeprefix('baseline-') for s in baseline_labels])
        keys = [time_static_label]
        keys.extend(baseline_labels)
        write_to_csv(filename, headers, keys, data)


def fast_vs_twol(collection: pymongo.collection.Collection):
    time_static_label = PREFIX + 'ts'
    time_dynamic_label = PREFIX + 'td'
    twol_label = PREFIX + 'twol'
    datasets = ['bms-pos-dedup-raw.txt', 'dblpv14', 'kosarak-dedup-raw.txt', 'lnonis1']

    for dataset in datasets:
        data = {}
        ts_result = queries.average_time(collection, time_static_label, dataset, 'jaccard')
        for result in ts_result:
            threshold = result['_id']['threshold']
            avg_join_time = result['average_join_time']
            avg_build_time = result.get('average_build_time', 0)

            if threshold not in data:
                data[threshold] = {}
            data[threshold]['time-static'] = avg_join_time
            data[threshold]['time-static-with-build'] = avg_join_time + avg_build_time
        
        ts_ratio = queries.algorithm_ratio(collection, time_static_label, dataset, 'jaccard')
        for result in ts_ratio:
            data[result['threshold']]['time-static-palloc-ratio'] = result['palloc']

        td_result = queries.average_time(collection, time_dynamic_label, dataset, 'jaccard')
        for result in td_result:
            threshold = result['_id']['threshold']
            average_total_time = result['average_join_time']

            if threshold not in data:
                data[threshold] = {}
            data[threshold]['time-dynamic'] = average_total_time

        twol_result = queries.average_time(collection, twol_label, dataset, 'jaccard')
        for result in twol_result:
            threshold = result['_id']['threshold']
            average_total_time = result['average_total_time']

            if threshold not in data:
                data[threshold] = {}
            data[threshold]['twol'] = average_total_time
        twol_ratio = queries.algorithm_ratio_twol(collection, twol_label, dataset, 'jaccard')
        for result in twol_ratio:
            data[result['threshold']]['twol-palloc-ratio'] = result['palloc']

        filename = f'vstwol/{remove_extension(dataset)}.csv'
        headers = ['Threshold', 'time-static', 'time-static-with-build', 'time-static-palloc-ratio', 'time-dynamic', 'twol', 'twol-palloc-ratio']
        keys = ['time-static', 'time-static-with-build', 'time-static-palloc-ratio', 'time-dynamic', 'twol', 'twol-palloc-ratio']
        write_to_csv(filename, headers, keys, data)


def fast_vs_limes(collection: pymongo.collection.Collection):
    time_static_label = PREFIX + 'jaro-ts'
    limes_label = PREFIX + 'jaro-limeslight'
    datasets = ['company_names', 'dblp', 'enron', 'trec', 'word']

    for dataset in datasets:
        data = {}
        ts_result = queries.average_time(collection, time_static_label, dataset, 'jaro')
        for result in ts_result:
            threshold = result['_id']['threshold']
            avg_join_time = result['average_join_time']
            avg_build_time = result.get('average_build_time', 0)

            if threshold not in data:
                data[threshold] = {}
            data[threshold]['time-static'] = avg_join_time + avg_build_time

        limes_result = queries.average_time(collection, limes_label, dataset, 'jaro')
        for result in limes_result:
            threshold = result['_id']['threshold']
            average_join_time = result['average_join_time']

            if threshold not in data:
                data[threshold] = {}
            data[threshold]['limes'] = average_join_time

        filename = f'vslimes/{remove_extension(dataset)}.csv'
        headers = ['Threshold', 'time-static', 'limes']
        keys = ['time-static', 'limes']
        write_to_csv(filename, headers, keys, data)

def main():
    uri = config.db_config['connection_string']

    # Create a new client and connect to the server
    client = MongoClient(uri, server_api=ServerApi('1'))
    database = client.get_database(config.db_config['database'])
    collection = database.get_collection(config.db_config['collection'])

    set_datasets = ['bms-pos-dedup-raw.txt', 'kosarak-dedup-raw.txt', 'livejournal-userswithgroups-raw.txt',
                    'orkut-userswithgroups-dedup-raw.txt', 'dblpv14', 'lnonis1']
    string_datasets = ['dblp', 'enron', 'trec', 'word']
    tree_datasets = ['sentiment', 'python', 'swissprot', 'synthetic', 'dblp']

    static_vs_dynamic(set_datasets, 'jaccard', collection)
    static_vs_dynamic(string_datasets, 'sed', collection)
    static_vs_dynamic(tree_datasets, 'ted', collection)

    # static_vs_baseline(set_datasets, 'jaccard', collection)
    # static_vs_baseline(set_datasets, 'sed', collection)
    # static_vs_baseline(set_datasets, 'ted', collection)

    fast_vs_twol(collection)
    fast_vs_limes(collection)


if __name__ == '__main__':
    main()

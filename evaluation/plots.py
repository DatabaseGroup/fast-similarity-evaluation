from collections import defaultdict

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


class GapStats:
    def __init__(self):
        self.sum_for_label = defaultdict(float)
        self.count_for_label = defaultdict(int)

    def record(self, labels: list[str], values: dict[str, float], expected_best: str):
        if expected_best in values:
            best = values[expected_best]
            for label in labels:
                if label in values:
                    value = values[label] / best
                    self.sum_for_label[label] += value
                    self.count_for_label[label] += 1


    def get_averages(self) -> list[tuple[str, float]]:
        res = []
        for label, value in self.sum_for_label.items():
            res.append((label, value / self.count_for_label[label]))
        res.sort(key=lambda k: k[1])
        return res

    def print(self):
        avgs = self.get_averages()
        for avg in avgs:
            print(f'{avg[0]}: {avg[1]}')

def static_vs_dynamic(datasets: list[str], similarity: str, collection: pymongo.collection.Collection, stats: GapStats, prefix=PREFIX, fileprefix='time'):
    time_static_label = prefix + 'ts'
    time_dynamic_label = prefix + 'td'
    time_dynamic_warmup = prefix + 'tdw'
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

        filename = f'{fileprefix}/{remove_extension(dataset)}-{similarity}.csv'
        headers = ['Threshold', 'time-static', 'time-dynamic', 'time-dynamic-warmup', 'time-static-with-build']
        keys = [time_static_label, time_dynamic_label, time_dynamic_warmup, 'time-static-with-build']
        write_to_csv(filename, headers, keys, data)

        for threshold in data.values():
            stats.record(keys, threshold, time_static_label)


class BaselineAccStats:
    def __init__(self):
        self.total_ratio = 0.
        self.ratio_count = 0
        self.max_ratio = 0.
        self.max_ratio_per_label = defaultdict(float)

    def record(self, ratio: float, label: str):
        self.max_ratio_per_label[label] = max(self.max_ratio_per_label[label], ratio)
        self.total_ratio += ratio
        self.ratio_count += 1
        self.max_ratio = max(self.max_ratio, ratio)

    def avg_ratio(self) -> float:
        return self.total_ratio / self.ratio_count

def static_vs_baseline(datasets: list[str], similarity: str, collection: pymongo.collection.Collection, stats: BaselineAccStats):
    time_static_label = PREFIX + 'ts'
    set_baselines = ['baseline-prefix', 'baseline-palloc']
    string_baselines = ['baseline-qgram-prefix', 'baseline-qgram-palloc', 'baseline-passjoin']
    tree_baselines = ['baseline-tjoin', 'baseline-labelset-palloc', 'baseline-labelset-prefix',
                      'baseline-traversal-prefix', 'baseline-traversal-palloc', 'baseline-traversal-passjoin']

    set_baselines = [PREFIX + s for s in set_baselines]
    string_baselines = [PREFIX + s for s in string_baselines]
    tree_baselines = [PREFIX + s for s in tree_baselines]

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
            avg_build_time = result['average_build_time']

            if threshold not in data:
                data[threshold] = {}

            data[threshold][result['_id']['label']] = avg_join_time
            data[threshold][result['_id']['label'] + '-with-build'] = avg_join_time + avg_build_time

        filename = f'baseline/{remove_extension(dataset)}-{similarity}.csv'
        headers = ['Threshold', 'time-static']
        headers.extend([s.removeprefix(f'{PREFIX}baseline-') for s in baseline_labels])
        headers.extend([s.removeprefix(f'{PREFIX}baseline-') + '-with-build' for s in baseline_labels])
        keys = [time_static_label]
        keys.extend(baseline_labels)
        keys.extend([s + '-with-build' for s in baseline_labels])
        write_to_csv(filename, headers, keys, data)

        for threshold in data.keys():
            ts_time = data[threshold][time_static_label]
            max_ratio = 0
            for key, item in data[threshold].items():
                ratio = ts_time / item
                max_ratio = max(ratio, max_ratio)

            stats.record(max_ratio, dataset)


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

        filename = f'vstwol/{remove_extension(dataset)}-jaccard.csv'
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

def index_redundancy(datasets: list[str], similarity: str, collection: pymongo.collection.Collection, stats: GapStats):
    time_static_label = PREFIX + 'ts'
    time_dynamic_label = PREFIX + 'td'

    for dataset in datasets:
        result = queries.fast_index_redundancy(collection, time_static_label, time_dynamic_label, dataset, similarity)

        filename = f'index_redundancy/{remove_extension(dataset)}-{similarity}.csv'
        headers = ['Threshold', 'time-static', 'time-dynamic']
        keys = ['time-static', 'time-dynamic']
        write_to_csv(filename, headers, keys, result)

        for threshold in result.values():
            stats.record(keys, threshold, 'time-static')


def fast_vs_syncsignatures(collection: pymongo.collection.Collection):
    ss_ejoin_label = PREFIX + 'syncsig-ejoin'
    ss_bjoin_label = PREFIX + 'syncsig-bjoin'

    time_static_label = PREFIX + 'ts'
    time_static_partition_label = PREFIX + 'ts-partitiononly'
    time_dynamic_label = PREFIX + 'td-withpartition'
    time_dynamic_warmup_label = PREFIX + 'tdw-withpartition'

    datasets = ['jscript1k', 'python1k', 'swissprot1k']

    for dataset in datasets:
        data = defaultdict(lambda: defaultdict(dict))
        headers = ['Threshold']
        keys = []

        for label in (ss_ejoin_label, ss_bjoin_label):
            alg = 'ejoin' if 'ejoin' in label else 'bjoin'
            headers.append(alg)
            keys.append(alg)
            results = queries.average_time(collection, label, dataset, 'ted')

            for result in results:
                threshold = result['_id']['threshold']
                avg_total_time = result['average_total_time']
                data[threshold][alg] = avg_total_time

        for label in (time_static_label, time_static_partition_label, time_dynamic_label, time_dynamic_warmup_label):
            if 'partitiononly' in label:
                alg = 'ts-partition'
            elif 'tdw' in label:
                alg = 'time-dynamic-warmup'
            elif 'td' in label:
                alg = 'time-dynamic'
            else:
                alg = 'time-static'
            headers.append(alg)
            keys.append(alg)
            results = queries.average_time(collection, label, dataset, 'ted')

            for result in results:
                threshold = result['_id']['threshold']
                avg_total_time = result['average_join_time'] + (result['average_build_time'] if result['average_build_time'] else 0)
                data[threshold][alg] = avg_total_time

        filename = f'syncsig/{remove_extension(dataset)}.csv'
        write_to_csv(filename, headers, keys, data)


def fast_vs_minjoin(our_datasets: list[str], collection: pymongo.collection.Collection):
    time_static_labels = (PREFIX + 'minjoin-ts-hq', PREFIX + 'minjoin-ts-mq', PREFIX + 'minjoin-ts-lq')
    minjoin_label = PREFIX + 'minjoin'

    their_datasets = ['uniref', 'minjtrec', 'gen50ks', 'gen20kl']
    all_datasets = [*our_datasets, *their_datasets]

    for dataset in all_datasets:
        data = defaultdict(lambda: defaultdict(dict))
        headers = ['Threshold', 'time-static-min', 'result_size', 'minjoin_result', 'minjoin_recall']
        keys = ['time-static-min', 'result_size', 'minjoin_result', 'minjoin_recall']

        for label in time_static_labels:
            alg = label.lstrip(PREFIX)
            keys.append(alg)
            headers.append(alg)

            results = queries.average_time(collection, label, dataset, 'sed')
            for result in results:
                threshold = result['_id']['threshold']
                if 'time-static-min' not in data[threshold]:
                    data[threshold]['time-static-min'] = 10**10
                data[threshold][alg] = result['average_join_time']
                data[threshold]['time-static-min'] = min(data[threshold]['time-static-min'], data[threshold][alg])
                data[threshold]['result_size'] = result['result_size']

        for label in (minjoin_label, ):
            alg = 'minjoin'
            keys.append(alg)
            headers.append(alg)

            results = queries.average_time(collection, label, dataset, 'sed')
            for result in results:
                threshold = result['_id']['threshold']
                data[threshold][alg] = result['average_total_time']
                data[threshold]['minjoin_result'] = result['result_size']
                data[threshold]['minjoin_recall'] = round(data[threshold]['minjoin_result'] / data[threshold]['result_size'] * 100, 1)
                if data[threshold]['minjoin_recall'] >= 99.9999:
                    data[threshold]['minjoin_recall'] = 100

        filename = f'minjoin/{remove_extension(dataset)}-sed.csv'
        write_to_csv(filename, headers, keys, data)


def main():
    uri = config.db_config['connection_string']

    # Create a new client and connect to the server
    client = MongoClient(uri, server_api=ServerApi('1'))
    database = client.get_database(config.db_config['database'])
    collection = database.get_collection(config.db_config['collection'])

    set_datasets = ['bms-pos-dedup-raw.txt', 'kosarak-dedup-raw.txt', 'dblpv14', 'lnonis1']
    string_datasets = ['dblp', 'enron', 'trec', 'word']
    tree_datasets = ['sentiment', 'python', 'swissprot', 'dblp']

    static_dynamic_stats = GapStats()
    hights_stats = GapStats()
    baseline_stats = BaselineAccStats()
    index_redundancy_stats = GapStats()

    for dataset, similarity in [(set_datasets, 'jaccard'), (string_datasets, 'sed'), (tree_datasets, 'ted')]:
        static_vs_dynamic(dataset, similarity, collection, static_dynamic_stats)
        static_vs_baseline(dataset, similarity, collection, baseline_stats)
        index_redundancy(dataset, similarity, collection, index_redundancy_stats)
        print(f'index redundancy gap factors ({dataset}):')
        index_redundancy_stats.print()
        index_redundancy_stats = GapStats()

        static_vs_dynamic(dataset, similarity, collection, hights_stats, f'{PREFIX}hights-', 'hights')

    print(f'avg ratio: {baseline_stats.avg_ratio()}')

    print('dynamic gap factors:')
    static_dynamic_stats.print()

    fast_vs_twol(collection)
    fast_vs_limes(collection)
    fast_vs_syncsignatures(collection)
    fast_vs_minjoin(string_datasets, collection)


if __name__ == '__main__':
    main()

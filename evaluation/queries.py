import pymongo.collection
from pymongo.mongo_client import MongoClient
from pymongo.server_api import ServerApi


def average_time(collection: pymongo.collection.Collection, label: str, dataset: str, similarity: str):
    pipeline = [
        {
            '$match': {
                'meta.label': label,
                'meta.dataset': dataset,
                'meta.similarity': similarity
            }
        },
        {
            '$group': {
                '_id': {
                    'dataset': '$meta.dataset',
                    'similarity': '$meta.similarity',
                    'threshold': '$meta.threshold',
                    'label': '$meta.label'
                },
                'average_join_time': {'$median': {'input': '$timing.join_time', 'method': 'approximate'}},
                'average_build_time': {'$median': {'input': '$timing.build_time', 'method': 'approximate'}},
                'average_total_time': {'$median': {'input': '$timing.total_time', 'method': 'approximate'}},
                'result_size': {'$avg': '$global_statistics.result_size'}
            }
        }, {
            '$project': {
                'dataset': '$_id.dataset',
                'similarity': '$_id.similarity',
                'threshold': '$_id.threshold',
                'label': '$_id.label',
                'average_join_time': 1,
                'average_build_time': 1,
                'average_total_time': 1,
                'average_result_size': 1,
                'result_size': 1,
                '_id': 0
            }
        }
    ]

    results = collection.aggregate(pipeline)
    return results


def algorithm_ratio(collection: pymongo.collection.Collection, label: str, dataset: str, similarity: str) -> list[dict]:
    pipeline = [
        {
            '$match': {
                'meta.label': label,
                'meta.dataset': dataset,
                'meta.similarity': similarity
            }
        }, {
            '$project': {
                'threshold': '$meta.threshold',
                'local_statistics': {
                    '$objectToArray': '$local_statistics'
                }
            }
        }, {
            '$unwind': {
                'path': '$local_statistics'
            }
        }, {
            '$group': {
                '_id': {
                    'id': '$_id',
                    'threshold': '$threshold'
                },
                'total_selection_count': {
                    '$sum': '$local_statistics.v.selection_count'
                },
                'algorithms': {
                    '$push': {
                        'algorithm': '$local_statistics.k',
                        'selection_count': '$local_statistics.v.selection_count'
                    }
                }
            }
        }, {
            '$addFields': {
                'algorithms': {
                    '$map': {
                        'input': '$algorithms',
                        'as': 'algorithm',
                        'in': {
                            'algorithm': '$$algorithm.algorithm',
                            'selection_count': '$$algorithm.selection_count',
                            'relative_selection_count': {
                                '$divide': [
                                    '$$algorithm.selection_count', '$total_selection_count'
                                ]
                            }
                        }
                    }
                }
            }
        }, {
            '$unwind': {
                'path': '$algorithms'
            }
        }, {
            '$group': {
                '_id': {
                    'threshold': '$_id.threshold',
                    'algorithm': '$algorithms.algorithm'
                },
                'average_relative_selection_count': {
                    '$avg': '$algorithms.relative_selection_count'
                }
            }
        }, {
            '$group': {
                '_id': '$_id.threshold',
                'algorithms': {
                    '$push': {
                        'algorithm': '$_id.algorithm',
                        'average_relative_selection_count': '$average_relative_selection_count'
                    }
                }
            }
        }
    ]

    results = collection.aggregate(pipeline)

    plain = []
    for result in results:
        doc = {
            'threshold': result['_id'],
        }
        for alg in result['algorithms']:
            name = alg['algorithm']
            doc[name] = alg['average_relative_selection_count']

        plain.append(doc)

    return plain


def algorithm_ratio_twol(collection: pymongo.collection.Collection, label: str, dataset: str, similarity: str) -> list[
    dict]:
    pipeline = [
        {
            '$match': {
                'meta.label': label,
                'meta.dataset': dataset,
                'meta.similarity': similarity
            }
        }, {
            '$project': {
                '_id': '$meta.threshold',
                'palloc_ratio': {
                    '$divide': [
                        '$index.deleted_sets', '$index.collection_size'
                    ]
                }
            }
        }
    ]

    results = collection.aggregate(pipeline)

    plain = []
    for result in results:
        doc = {
            'threshold': result['_id'],
            'palloc': result['palloc_ratio']
        }

        plain.append(doc)

    return plain

def fast_index_redundancy(collection: pymongo.collection.Collection, static_label: str, dynamic_label: str, dataset: str, similarity: str):
    pipeline = [
        {
            '$match': {
                'meta.dataset': dataset,
                'meta.similarity': similarity,
                'meta.label': {
                    '$in': [
                        static_label, dynamic_label
                    ]
                }
            }
        }, {
            '$project': {
                'mode': '$meta.mode',
                'threshold': '$meta.threshold',
                'index_redundancy': '$global_statistics.indexed_ratio'
            }
        }, {
            '$group': {
                '_id': {
                    'mode': '$mode',
                    'threshold': '$threshold'
                },
                'avg_index_redundancy': {
                    '$median': {
                        'input': '$index_redundancy',
                        'method': 'approximate'
                    }
                }
            }
        }
    ]

    results = collection.aggregate(pipeline)

    plain = {}
    for result in results:
        mode = result['_id']['mode']
        threshold = result['_id']['threshold']
        index_redundancy = result['avg_index_redundancy']

        if threshold not in plain:
            plain[threshold] = {}
        plain[threshold][mode] = index_redundancy - 1

    return plain

def preprocessing_time(collection: pymongo.collection.Collection, dataset: str):
    pipeline = [
        {
            '$match': {
                'meta.label': 'preprocessing',
                'meta.dataset': dataset
            }
        }, {
            '$group': {
                '_id': 'meta.dataset',
                'avg_time': {
                    '$avg': '$timing.preprocessing'
                }
            }
        }, {
            '$project': {
                'dataset': '$_id',
                'avg_time': '$avg_time'
            }
        }
    ]

    results = collection.aggregate(pipeline)
    return results

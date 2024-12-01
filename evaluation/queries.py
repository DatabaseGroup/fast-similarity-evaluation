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
                'average_total_time': {'$median': {'input': '$timing.total_time', 'method': 'approximate'}}
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
                '_id': '$threshold',
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
        }
    ]

    results = collection.aggregate(pipeline)

    plain = []
    for result in results:
        doc = {
            'threshold': result['_id'],
        }
        total = result['total_selection_count']
        for alg in result['algorithms']:
            name = alg['algorithm']
            selection = alg['selection_count']
            doc[name] = selection / total

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

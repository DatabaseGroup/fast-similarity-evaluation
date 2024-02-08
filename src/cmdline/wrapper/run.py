#!/bin/env python3

import logging
import os
import sys
import subprocess

import pymongo as mng
import pymongo.database
from bson import json_util

from db import *
import config

# globals
mongo_client: mng.MongoClient
mongo_database: mng.database.Database
mongo_collection: mng.collection.Collection
logging.basicConfig()
logger = logging.getLogger("benchmark")
logger.setLevel(logging.INFO)


def main():
    global mongo_client, mongo_database, mongo_collection
    mongo_client, mongo_database, mongo_collection = connect_to_db(config.db_config)

    results = subprocess.run(sys.argv[1:], capture_output=True, text=True)
    result = json_util.loads(results.stdout)

    write_to_db(mongo_collection, result)


if __name__ == "__main__":
    main()

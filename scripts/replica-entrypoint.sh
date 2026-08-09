#!/bin/sh
set -e

if [ ! -s "/var/lib/postgresql/data/PG_VERSION" ]; then
    echo "Initializing replica from primary db..."
    until pg_isready -h db -p 5432 -U ${DB_USER:-postgres}; do
        echo "Waiting for primary db to be ready..."
        sleep 1
    done
    rm -rf /var/lib/postgresql/data/*
    PGPASSWORD=replpass pg_basebackup -h db -p 5432 -U replicator -D /var/lib/postgresql/data -Fp -Xs -P -R
    chown -R postgres:postgres /var/lib/postgresql/data
    chmod 700 /var/lib/postgresql/data
fi

exec su-exec postgres postgres -c hot_standby=on

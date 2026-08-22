#!/bin/bash
set -euo pipefail

# Fail-fast: the app must not serve traffic on an unmigrated schema.
if [ -d "./db" ]; then
    ./db/migrate.sh
fi

exec ./notenest

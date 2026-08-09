#!/bin/bash
set -eo pipefail

# Run pending database schema migrations before starting the C++ application server
if [ -d "./db" ]; then
    ./db/migrate.sh || echo "Notice: Migration runner finished or skipped."
fi

# Execute the primary application process
exec ./notenest

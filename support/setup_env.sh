#!/bin/bash
set -e

# Configuration
DEPS_DIR="$(pwd)/deps"
SQLITE_YEAR="2023"
SQLITE_VERSION="3440000" # 3.44.0

# URLs
SQLITE_URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip"

mkdir -p "$DEPS_DIR/src"
mkdir -p "$DEPS_DIR/include"
mkdir -p "$DEPS_DIR/lib"

function download_libs() {
    echo "[SETUP] Downloading dependencies..."
    
    # SQLite
    if [ ! -f "$DEPS_DIR/src/sqlite.zip" ]; then
        echo "  - Fetching SQLite..."
        wget -q -O "$DEPS_DIR/src/sqlite.zip" "$SQLITE_URL"
    fi

}

function build_sqlite() {
    echo "[SETUP] Building SQLite..."
    cd "$DEPS_DIR/src"
    unzip -q -o sqlite.zip
    cd "sqlite-amalgamation-${SQLITE_VERSION}"
    
    # Compile static lib
    gcc -O2 -c sqlite3.c
    ar rcs "$DEPS_DIR/lib/libsqlite3.a" sqlite3.o
    
    # Copy headers
    cp sqlite3.h sqlite3ext.h "$DEPS_DIR/include/"
    echo "[SETUP] SQLite built successfully."
}


function main() {
    download_libs
    build_sqlite
    
    
    echo "--------------------------------------------------------"
    echo "Setup complete."
    echo "Libraries installed in: $DEPS_DIR"
    echo "Use 'make' to build against these libraries."
    echo "--------------------------------------------------------"
}

main

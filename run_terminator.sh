#!/usr/bin/env bash
# Terminator Launcher
# Allows launching Terminator directly by double-clicking in file managers

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
exec "$DIR/terminator" "$@"

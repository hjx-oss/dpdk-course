#!/usr/bin/env bash
set -euo pipefail
lesson_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "$lesson_dir/../env.sh"
make -C "$lesson_dir"
exec python3 "$lesson_dir/tests/demo.py" "$@"

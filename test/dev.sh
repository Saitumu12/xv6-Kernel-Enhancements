#!/usr/bin/env bash
set -eu
IMAGE=xv6-dev
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec docker run --rm -it -v "${ROOT}:/src" "$IMAGE" "$@"

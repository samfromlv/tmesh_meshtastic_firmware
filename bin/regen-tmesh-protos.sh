#!/usr/bin/env bash
set -e

cd "$(dirname "$0")/.."
nanopb-0.4.9/generator-bin/protoc -Isrc/tmesh --nanopb_out=-S.cpp:src/tmesh src/tmesh/feature_flags.proto

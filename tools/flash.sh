#!/usr/bin/env bash
# Build and upload an ESPHome YAML using the official esphome docker image.
#
# Usage:
#   tools/flash.sh                          # OTA to examples/basic.yaml
#   tools/flash.sh examples/debug.yaml      # OTA to another YAML
#   tools/flash.sh examples/basic.yaml /dev/ttyUSB0   # serial upload
#
# The script always mounts the repo root at /config so the YAML can refer
# to ./components and ./secrets.yaml normally. --network host is needed
# for OTA / mDNS (.local) discovery.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
YAML_REL="${1:-examples/basic.yaml}"
DEVICE="${2:-}"

IMAGE="${ESPHOME_IMAGE:-ghcr.io/esphome/esphome:latest}"

DOCKER_ARGS=(
  --rm -it
  --network host
  -v "${REPO_ROOT}:/config"
  -w /config
)

ESPHOME_ARGS=( run "${YAML_REL}" )

if [[ -n "${DEVICE}" ]]; then
  if [[ ! -e "${DEVICE}" ]]; then
    echo "error: device ${DEVICE} not found" >&2
    exit 1
  fi
  DOCKER_ARGS+=( --device "${DEVICE}:${DEVICE}" )
  ESPHOME_ARGS+=( --device "${DEVICE}" )
fi

echo ">>> docker run ${IMAGE} esphome ${ESPHOME_ARGS[*]}"
exec docker run "${DOCKER_ARGS[@]}" "${IMAGE}" "${ESPHOME_ARGS[@]}"

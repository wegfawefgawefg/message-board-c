#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SERVER_PID=""

snapshot_tree() {
  (
    cd "${ROOT_DIR}"
    {
      find src assets -type f -print0 2>/dev/null || true
      printf '%s\0' CMakeLists.txt
    } | xargs -0 -r stat -c '%n %Y %s' 2>/dev/null | sort | sha256sum | awk '{print $1}'
  )
}

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  pkill -f "^${BUILD_DIR}/message_board$" 2>/dev/null || true
  SERVER_PID=""
}

start_server() {
  echo "[dev]\tstarting server"
  if [[ -t 0 ]]; then
    "${BUILD_DIR}/message_board" </dev/tty &
    SERVER_PID=$!
  else
    (tail -f /dev/null | "${BUILD_DIR}/message_board") &
    SERVER_PID=$!
  fi
}

cleanup() {
  stop_server
}
trap cleanup EXIT INT TERM

echo "[dev]\tinitial build"
"${ROOT_DIR}/scripts/build.sh"
start_server

LAST_SNAPSHOT="$(snapshot_tree)"
echo "[dev]\twatching for changes"

while true; do
  sleep 1
  CURRENT_SNAPSHOT="$(snapshot_tree)"
  if [[ "${CURRENT_SNAPSHOT}" == "${LAST_SNAPSHOT}" ]]; then
    continue
  fi

  LAST_SNAPSHOT="${CURRENT_SNAPSHOT}"
  echo "[dev]\tchange detected; rebuilding"
  if "${ROOT_DIR}/scripts/build.sh"; then
    stop_server
    start_server
  else
    echo "[dev]\tbuild failed; keeping previous server state"
  fi
done

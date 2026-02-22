#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_URL="${1:-http://127.0.0.1:8888}"
COUNT="${2:-128}"

adjectives=(brisk mellow neon fuzzy wired cosmic rusty lucid)
nouns=(otter falcon pine comet fox tide ember quartz)
verbs=(ships tests debugs tunes patches maps profiles traces)
shared_nicks=(cow fox owl emberbot)

rand_from() {
  local -n arr=$1
  echo "${arr[RANDOM % ${#arr[@]}]}"
}

for ((i=1; i<=COUNT; i++)); do
  if (( RANDOM % 100 < 45 )); then
    nick="$(rand_from shared_nicks)"
    client_slot=$((RANDOM % 4))
    client_id="seed-${nick}-client-${client_slot}"
  else
    nick="$(rand_from adjectives)_$(rand_from nouns)$((RANDOM % 90 + 10))"
    client_id="seed-$(printf '%08x' "$RANDOM")-$(printf '%08x' "$RANDOM")"
  fi

  msg="$(rand_from nouns) $(rand_from verbs) $(rand_from adjectives) build #${i}"

  curl -fsS -X POST "${BASE_URL}/post" \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data-urlencode "nickname=${nick}" \
    --data-urlencode "client_id=${client_id}" \
    --data-urlencode "message=${msg}" \
    --data-urlencode "ajax=1" \
    >/dev/null

  if (( i % 16 == 0 )); then
    printf '[seed]\tposted %d/%d\n' "$i" "$COUNT"
  fi
done

printf '[seed]\tdone: posted %d messages to %s\n' "$COUNT" "$BASE_URL"

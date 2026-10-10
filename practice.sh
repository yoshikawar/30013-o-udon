#!/usr/bin/env bash

set -o pipefail
mkdir -p run

if [[ -z ${PROCON_TOKEN:-} ]]; then
  echo "PROCON_TOKEN is not set." >&2
  exit 1
fi

echo "Configuring and building Release client..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release || exit $?
cmake --build build --parallel 4 || exit $?

archive_previous_session() {
  local archive="run/session-previous-$(date +%Y%m%d-%H%M%S)"
  local suffix=1
  while [[ -e "$archive" ]]; do
    archive="run/session-previous-$(date +%Y%m%d-%H%M%S)-$suffix"
    ((suffix += 1))
  done

  mv run/session "$archive" || return $?
  echo "Previous match state archived to $archive."
}

prepare_session_for_match() {
  local state=run/session/session.json
  [[ -f "$state" ]] || return 0

  if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to inspect the saved session safely." >&2
    return 1
  fi

  local current_match_id saved_match_id
  current_match_id=$(jq -er '
    (.map.height | tostring) + "x" + (.map.width | tostring) + ":" +
    (.agents | map(tostring) | join(",")) + ","
  ' run/setting-response.json) || {
    echo "Cannot determine the current match identity from /setting." >&2
    return 1
  }
  saved_match_id=$(jq -er '.matchId' "$state") || {
    echo "Cannot determine the saved session identity; keeping it for manual inspection." >&2
    return 1
  }

  [[ "$saved_match_id" == "$current_match_id" ]] && return 0

  # The current writer always writes submissionAttempted.  Older session files
  # did not, so an Accepted record is considered known only when its HTTP
  # response and revision prove that the server accepted it.
  if ! jq -e '
    (.agentKindsUnknown == true) or
    any(.submissions[]?;
      (.classification == 5) or
      ((.submissionAttempted == null) and
       (has("submissionAttempted") or
        .classification != 0 or .httpStatus != 200 or .revision == null)))
  ' "$state" >/dev/null; then
    archive_previous_session
    return $?
  fi

  echo "Previous match state has an unknown POST outcome; keeping run/session for manual inspection." >&2
  return 1
}

# you can custom waiting time(limited 180)
echo "Waiting for the match setting (up to 180 seconds)..."
setting_deadline=$((SECONDS + 180))
while :; do
  : > run/setting-response.json
  http_status=$(curl -sS \
    --connect-timeout 5 \
    --max-time 10 \
    -o run/setting-response.json \
    -w '%{http_code}' \
    -G --data-urlencode "token=${PROCON_TOKEN}" \
    "http://172.28.0.10:8080/setting")
  curl_status=$?

  if [[ $curl_status -ne 0 ]]; then
    echo "Setting request failed (curl status $curl_status)." >&2
  elif [[ $http_status == 200 ]]; then
    echo "Match setting is available; starting client."
    break
  elif [[ $http_status == 401 ]]; then
    echo "Authentication failed. Check PROCON_TOKEN." >&2
    exit 1
  elif [[ $http_status != 403 ]]; then
    echo "Unexpected /setting response: HTTP $http_status" >&2
    cat run/setting-response.json >&2
    exit 1
  fi

  if (( SECONDS >= setting_deadline )); then
    echo "Timed out waiting for the match setting." >&2
    if [[ -s run/setting-response.json ]]; then
      cat run/setting-response.json >&2
    fi
    exit 1
  fi
  sleep 1
done

prepare_session_for_match || exit $?

run_client() {
  ./build/hexa_udon auto \
    --base-url "http://172.28.0.10:8080" \
    --token-env PROCON_TOKEN \
    --max-get-retries 200 \
    --execute \
    2>&1 | tee run/client-output.log
}

run_client
client_status=$?

exit "$client_status"

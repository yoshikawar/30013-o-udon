#!/usr/bin/env bash
# 試合に 1 回出る。build → /setting を待つ → 前の試合の Session を退避 → hexa_udon auto --execute
#
#   ./practice.sh                     公式の試合（既定 http://172.28.0.10:8080、VENUE_BASE_URL で変えられる）
#                                     token は PROCON_TOKEN
#   ./practice.sh --arena             練習場（https://procon37arena.online）
#                                     token は PROCON_ARENA_TOKEN（公式の token を外へ送らないため別にする）
#   ./practice.sh --base-url URL      ほかのサーバー（token は PROCON_TOKEN）
#   ./practice.sh --wait 600          /setting を待つ秒数（既定 180）
#   ほかの引数はそのまま hexa_udon auto に渡す（例: ./practice.sh --threads 8）
#   token などは .env に書いておける（.env.example を .env にコピーして書き換える。.env は Git に入らない）

set -o pipefail
cd "$(dirname "$0")" || exit 1
mkdir -p run

# .env（このスクリプトと同じフォルダー）から KEY=値 の行を読む。すでに設定されている環境変数はそのまま。
# シェルとしては実行しない（source しない）ので、値に書いたコマンドは実行されない
load_env_file() {
  local file=$1 line key value
  [[ -f $file ]] || return 0
  if [[ $(stat -c '%a' "$file" 2>/dev/null || stat -f '%Lp' "$file") != 600 ]]; then
    echo "warning: $file can be read by other users (chmod 600 $file)" >&2
  fi
  while IFS= read -r line || [[ -n $line ]]; do
    line=${line%$'\r'}
    [[ $line =~ ^[[:space:]]*(export[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]] || continue
    key=${BASH_REMATCH[2]}
    value=${BASH_REMATCH[3]}
    if [[ $value =~ ^\"(.*)\"$ || $value =~ ^\'(.*)\'$ ]]; then value=${BASH_REMATCH[1]}; fi
    [[ -n ${!key+x} ]] || export "$key=$value"
  done < "$file"
}
load_env_file .env

BASE_URL="${VENUE_BASE_URL:-http://172.28.0.10:8080}"
TOKEN_ENV=PROCON_TOKEN
WAIT_SECONDS=180
extra_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arena) BASE_URL="https://procon37arena.online"; TOKEN_ENV=PROCON_ARENA_TOKEN; shift ;;
    --base-url) BASE_URL="${2:?--base-url needs a URL}"; shift 2 ;;
    --wait) WAIT_SECONDS="${2:?--wait needs seconds}"; shift 2 ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) extra_args+=("$1"); shift ;;
  esac
done
BASE_URL="${BASE_URL%/}"

if [[ -z ${!TOKEN_ENV:-} ]]; then
  echo "$TOKEN_ENV is not set." >&2
  exit 1
fi
TOKEN="${!TOKEN_ENV}"
echo "Target: $BASE_URL (token from $TOKEN_ENV)"

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

  # 別の試合の Session は、結果の分からない提出が残っていても退避する（消さずに残すので後から調べられる）
  if jq -e '(.agentKindsUnknown == true) or any(.submissions[]?; .submissionAttempted == null)' \
      "$state" >/dev/null; then
    echo "Previous match state has an unknown POST outcome (kept in the archive)." >&2
  fi
  archive_previous_session
}

echo "Waiting for the match setting (up to $WAIT_SECONDS seconds)..."
setting_deadline=$((SECONDS + WAIT_SECONDS))
while :; do
  : > run/setting-response.json
  http_status=$(curl -sS \
    --connect-timeout 5 \
    --max-time 10 \
    -o run/setting-response.json \
    -w '%{http_code}' \
    -G --data-urlencode "token=${TOKEN}" \
    "${BASE_URL}/setting")
  curl_status=$?

  if [[ $curl_status -ne 0 ]]; then
    echo "Setting request failed (curl status $curl_status)." >&2
  elif [[ $http_status == 200 ]]; then
    echo "Match setting is available; starting client."
    break
  elif [[ $http_status == 401 ]]; then
    echo "Authentication failed. Check $TOKEN_ENV." >&2
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
    --base-url "$BASE_URL" \
    --token-env "$TOKEN_ENV" \
    --max-get-retries 200 \
    --execute \
    "${extra_args[@]}" \
    2>&1 | tee run/client-output.log
}

run_client
client_status=$?

exit "$client_status"

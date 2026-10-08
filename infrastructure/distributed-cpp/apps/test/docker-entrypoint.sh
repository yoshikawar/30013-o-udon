#!/bin/sh
set -eu

case "${ROLE:-}" in
worker)
    exec hexa_udon worker \
        --listen "0.0.0.0:${PORT:-39001}" \
        --worker-token-env HEXA_LAN_WORKER_SECRET \
        --worker-index "${WORKER_INDEX:-0}" \
        --worker-count "${WORKER_COUNT:-1}" \
        --run-id "${RUN_ID:-docker}"
    ;;
main)
    set -- auto --base-url "${VENUE_BASE_URL:?Set VENUE_BASE_URL}" \
        --token-env PROCON_TOKEN \
        --session-dir /state/session --log-dir /state/log \
        --seed "${SEED:-30013}" \
        --safety-seconds "${SAFETY_SECONDS:-3}"
    if [ "${EXECUTE:-false}" = "true" ]; then set -- "$@" --execute; fi
    if [ -n "${LAN_WORKER_1:-}" ]; then set -- "$@" --lan-worker "$LAN_WORKER_1"; fi
    if [ -n "${LAN_WORKER_2:-}" ]; then set -- "$@" --lan-worker "$LAN_WORKER_2"; fi
    exec hexa_udon "$@"
    ;;
*)
    exec hexa_udon "$@"
    ;;
esac

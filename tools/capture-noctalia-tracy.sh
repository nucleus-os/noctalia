#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
nucleus_workspace="${NUCLEUS_WORKSPACE_PATH:-$(cd "$root/../nucleus-workspace" 2>/dev/null && pwd || true)}"
binary="$root/build-tracy-release/noctalia"
ipc_client="$root/build-baseline-release/noctalia"
seconds=15
port="${TRACY_PORT:-8086}"
validation=0
output=""

usage() {
  echo "usage: $0 [--binary PATH] [--seconds N] [--port N] [--output DIR] [--validation]" >&2
}

while (($# > 0)); do
  case "$1" in
    --binary) binary="$2"; shift 2 ;;
    --seconds) seconds="$2"; shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --output) output="$2"; shift 2 ;;
    --validation) validation=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if [[ -z "$nucleus_workspace" || ! -x "$nucleus_workspace/tools/nucleus" ]]; then
  echo "Nucleus workspace not found; set NUCLEUS_WORKSPACE_PATH" >&2
  exit 1
fi
if [[ ! -x "$binary" ]]; then
  echo "Tracy-enabled Noctalia binary not found: $binary" >&2
  exit 1
fi
if [[ ! -x "$ipc_client" ]]; then
  echo "Noctalia IPC client not found: $ipc_client" >&2
  exit 1
fi
if pgrep -x noctalia >/dev/null; then
  echo "Noctalia is already running; stop it before starting an isolated capture" >&2
  exit 1
fi
if [[ -n "$(ss -ltnH "sport = :$port" 2>/dev/null)" ]]; then
  echo "TCP port $port is already in use; select another with --port" >&2
  exit 1
fi

"$nucleus_workspace/tools/nucleus" profile receivers
receiver="$nucleus_workspace/compositor/.tracy-build/tracy-capture"
exporter="$nucleus_workspace/compositor/.tracy-build/tracy-csvexport"
if [[ ! -x "$receiver" || ! -x "$exporter" ]]; then
  echo "Nucleus Tracy receivers were not produced" >&2
  exit 1
fi

if [[ -z "$output" ]]; then
  output="$root/profiles/noctalia-$(date +%Y%m%d-%H%M%S)"
elif [[ "$output" != /* ]]; then
  output="$root/$output"
fi
mkdir -p "$output"
capture="$output/capture.tracy"

capture_pid=""
app_pid=""
cleanup() {
  if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
    kill -TERM "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  if [[ -n "$capture_pid" ]] && kill -0 "$capture_pid" 2>/dev/null; then
    kill -INT "$capture_pid" 2>/dev/null || true
    wait "$capture_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

"$receiver" -o "$capture" -p "$port" -s "$seconds" >"$output/tracy-capture.log" 2>&1 &
capture_pid=$!
sleep 0.2

validation_env=()
if ((validation)); then
  validation_env+=(NOCTALIA_VULKAN_VALIDATION=1)
fi
env TRACY_PORT="$port" "${validation_env[@]}" "$binary" >"$output/noctalia.log" 2>&1 &
app_pid=$!

opened=0
for _ in {1..150}; do
  if ! kill -0 "$app_pid" 2>/dev/null; then
    echo "Noctalia exited during startup; see $output/noctalia.log" >&2
    exit 1
  fi
  if "$ipc_client" msg panel-open apple-music >/dev/null 2>&1; then
    opened=1
    break
  fi
  sleep 0.1
done
if ((!opened)); then
  echo "Noctalia IPC did not become ready; see $output/noctalia.log" >&2
  exit 1
fi

set +e
wait "$capture_pid"
capture_status=$?
set -e
capture_pid=""
if ((capture_status != 0)); then
  echo "Tracy capture failed; see $output/tracy-capture.log" >&2
  exit "$capture_status"
fi

kill -TERM "$app_pid" 2>/dev/null || true
wait "$app_pid" 2>/dev/null || true
app_pid=""

"$exporter" "$capture" >"$output/zones.csv"
"$exporter" --unwrap --plot "$capture" >"$output/plots.csv"
printf '%s\n' \
  "binary=$binary" \
  "seconds=$seconds" \
  "port=$port" \
  "validation=$validation" \
  "tracy_source=$nucleus_workspace/swift-tracy/third-party/tracy" \
  >"$output/metadata.txt"

trap - EXIT INT TERM
echo "Tracy profile captured at $output"

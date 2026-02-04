#!/usr/bin/env bash
set -euo pipefail

action="show"
append_text=""

if [[ "${1:-}" == "--append" ]]; then
  action="append"
  append_text="${2:-}"
  shift 2
fi

workspace_path="${1:-$(pwd)}"
workspace_root="$(cd "$workspace_path" && pwd)"

if git -C "$workspace_root" rev-parse --show-toplevel >/dev/null 2>&1; then
  workspace_root="$(git -C "$workspace_root" rev-parse --show-toplevel)"
fi

workspace_name="$(basename "$workspace_root")"
base_dir="${TALK_HISTORY_BASE:-$HOME/work/talk_history}"
date_dir="$(date +%F)"
day_dir="$base_dir/$workspace_name/$date_dir"
log_file="$day_dir/chat.md"

mkdir -p "$day_dir"
touch "$log_file"

mkdir -p "$workspace_root/.talk_history"
echo "$log_file" > "$workspace_root/.talk_history/latest_path.txt"

if [[ "$action" == "append" ]]; then
  if [[ -n "$append_text" ]]; then
    printf "- %s %s\n" "$(date +%H:%M)" "$append_text" >> "$log_file"
  fi
fi

echo "Talk history path: $log_file"

if [[ -s "$log_file" ]]; then
  echo "----- Today -----"
  tail -n 200 "$log_file"
  exit 0
fi

previous_file=$(ls -1 "$base_dir/$workspace_name"/*/chat.md 2>/dev/null | sort | tail -n 1 || true)

if [[ -n "$previous_file" && "$previous_file" != "$log_file" && -s "$previous_file" ]]; then
  echo "----- Previous Day -----"
  tail -n 200 "$previous_file"
else
  echo "No previous memory yet."
fi

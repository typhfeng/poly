#!/usr/bin/env bash
set -euo pipefail

config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
user_dir="$config_home/Code/User"
tasks_file="$user_dir/tasks.json"
bin_dir="$HOME/.local/bin"
global_script="$bin_dir/talk_history.sh"

mkdir -p "$user_dir" "$bin_dir"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cp "$script_dir/talk_history.sh" "$global_script"
chmod +x "$global_script"

TASKS_FILE="$tasks_file" python3 - <<'PY'
import json
import os

tasks_file = os.environ["TASKS_FILE"]

task = {
    "label": "Load talk history",
    "type": "shell",
    "command": "${env:HOME}/.local/bin/talk_history.sh",
    "args": ["${workspaceFolder}"],
    "runOptions": {"runOn": "folderOpen"},
    "presentation": {"reveal": "always", "panel": "dedicated", "clear": True},
}

data = {"version": "2.0.0", "tasks": []}
if os.path.exists(tasks_file):
    try:
        with open(tasks_file, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except json.JSONDecodeError:
        backup = tasks_file + ".bak"
        os.replace(tasks_file, backup)
        data = {"version": "2.0.0", "tasks": []}

tasks = data.get("tasks", [])
if not any(item.get("label") == task["label"] for item in tasks if isinstance(item, dict)):
    tasks.append(task)

data["version"] = data.get("version", "2.0.0")
data["tasks"] = tasks

with open(tasks_file, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2)
    handle.write("\n")
PY

echo "Installed global VS Code task in: $tasks_file"
echo "Installed global script: $global_script"

#!/bin/bash
# Merge an incoming settings.json (from a config bundle import, step 2.2
# in config-export-import.md) with a target repo's existing
# settings.json, without dropping the target's own project-specific hook
# registrations. A plain overwrite would silently lose any hook group
# that mixes a bundle hook with a project-specific one, or that only
# registers project-specific hooks -- those files stay on disk (2.9
# already protects them) but with nothing left registering them, which
# is just as broken as deleting them.
#
# Rule: any hook group in the target whose commands are ALL among this
# bundle's known hook names is dropped (the incoming version replaces
# it). Any group containing at least one command NOT in the bundle's
# list is kept whole and appended under its event key.
#
# Usage: ./merge-settings.sh <target-settings.json> <incoming-settings.json> <hook1.sh> [hook2.sh ...]
# Output: merged settings.json on stdout
set -euo pipefail

if [ "$#" -lt 3 ]; then
  echo "Usage: $0 <target-settings.json> <incoming-settings.json> <hook1.sh> [hook2.sh ...]" >&2
  exit 1
fi

target="$1"
incoming="$2"
shift 2
bundle_hooks_json="$(printf '%s\n' "$@" | jq -R . | jq -s .)"

jq -s --argjson bundle_hooks "$bundle_hooks_json" '
  .[0] as $target | .[1] as $incoming |
  ($incoming.hooks) as $inc |
  ($target.hooks // {}) as $tgt |
  {
    hooks: (
      $inc + (
        $tgt | to_entries | map({
          key: .key,
          value: (.value | map(select(
            (.hooks // []) | any(.command as $c | $bundle_hooks | any($c | test(.)) | not)
          )))
        }) | map(select(.value | length > 0))
        | reduce .[] as $entry
            ({}; .[$entry.key] = (.[$entry.key] // []) + $entry.value)
      ) as $extra |
      (($inc | keys) + ($extra | keys) | unique) as $all_keys |
      reduce $all_keys[] as $k
        ({}; .[$k] = (($inc[$k] // []) + ($extra[$k] // [])))
    )
  }
' "$target" "$incoming"

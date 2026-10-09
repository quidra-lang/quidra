#!/usr/bin/env bash
# CI: clone the packages pinned in tests/golden/corpus.toml and write
# $QUIDRA_GOLDEN_HOME/sources.conf for them (README.md, Setup). A package whose
# repository or pinned commit cannot be fetched is left out; the gates then
# run with --allow-missing and skip its entries. Local runs list their own
# clones in sources.conf instead.
#
#   tests/golden/fetch_packages.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHON="${PYTHON:-python3}"
if [[ -z "${QUIDRA_GOLDEN_HOME:-}" ]]; then
  echo "fetch_packages.sh: set QUIDRA_GOLDEN_HOME" >&2
  exit 2
fi
packages="$QUIDRA_GOLDEN_HOME/packages"
mkdir -p "$packages"
: > "$QUIDRA_GOLDEN_HOME/sources.conf"

"$PYTHON" - "$REPO" <<'PY' > "$QUIDRA_GOLDEN_HOME/packages.tsv"
import sys
sys.path.insert(0, sys.argv[1] + "/scripts")
import toml_subset
for package in toml_subset.load(sys.argv[1] + "/tests/golden/corpus.toml").get("package", []):
    print(package["name"], package["repository"], package["commit"])
PY

while read -r name repository commit; do
  if [[ ! -d "$packages/$name/.git" ]] &&
     ! git clone --quiet --filter=blob:none "$repository" "$packages/$name"; then
    echo "fetch_packages.sh: $name: cannot clone $repository; its entries are skipped" >&2
    continue
  fi
  if ! git -C "$packages/$name" cat-file -e "$commit^{commit}" 2>/dev/null; then
    echo "fetch_packages.sh: $name: pinned commit $commit not in $repository; its entries are skipped" >&2
    continue
  fi
  echo "package $name $packages/$name" >> "$QUIDRA_GOLDEN_HOME/sources.conf"
done < "$QUIDRA_GOLDEN_HOME/packages.tsv"

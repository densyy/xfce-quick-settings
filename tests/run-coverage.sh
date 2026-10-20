#!/bin/sh
# Cobertura por arquivo de src/ usando gcov (JSON). Rode da raiz:
#   sh tests/run-coverage.sh
# Para worktrees em paralelo, use QS_COV_DIR=/tmp/qs-cov-<thread>.
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)
out=${QS_COV_DIR:-/tmp/qs-coverage}

PKGS="gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3"
CFLAGS="-Wall -Wextra -Wconversion -g -std=gnu11 -O0 --coverage -I$repo/src -I$repo/tests $(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lsqlite3"

rm -rf "$out"
mkdir -p "$out"

for t in tests/test-*.c; do
  name=$(basename "$t" .c)
  d="$out/$name"
  mkdir -p "$d"
  echo "== coverage: $name =="
  ( cd "$d" && cc $CFLAGS -o "$d/$name" "$repo/$t" "$repo"/src/*.c $LIBS )
  if ! ( cd "$repo" && "$d/$name" ) > "$d/run.log" 2>&1; then
    echo "FALHOU: $name (ver $d/run.log)"
    exit 1
  fi
  ( cd "$d" && gcov --json-format ./*.gcno >/dev/null 2>&1 ) || true
done

python3 - "$out" <<'PY'
import glob
import gzip
import json
import os
import sys
from collections import defaultdict

root = sys.argv[1]
lines = defaultdict(dict)
funcs = defaultdict(dict)

for path in glob.glob(os.path.join(root, '*', '*.gcov.json.gz')):
    with gzip.open(path, 'rt') as handle:
        data = json.load(handle)
    for entry in data.get('files', []):
        name = entry['file']
        if '/src/' not in name or not name.endswith('.c'):
            continue
        for line in entry.get('lines', []):
            number = line['line_number']
            lines[name][number] = max(lines[name].get(number, 0),
                                      line.get('count', 0))
        for func in entry.get('functions', []):
            funcs[name][func['name']] = max(funcs[name].get(func['name'], 0),
                                            func.get('execution_count', 0))

print()
print(f"{'arquivo':<26} {'linhas':>16} {'funções':>14}")
for name in sorted(lines):
    covered_lines = sum(1 for value in lines[name].values() if value > 0)
    total_lines = len(lines[name])
    covered_funcs = sum(1 for value in funcs[name].values() if value > 0)
    total_funcs = len(funcs[name])
    line_pct = 100.0 * covered_lines / total_lines if total_lines else 0.0
    func_pct = 100.0 * covered_funcs / total_funcs if total_funcs else 0.0
    print(f"{os.path.basename(name):<26} "
          f"{covered_lines:>5}/{total_lines:<5} {line_pct:>4.0f}%  "
          f"{covered_funcs:>4}/{total_funcs:<4} {func_pct:>3.0f}%")

minimum = os.environ.get('QS_COV_MIN')
if minimum:
    low = []
    for name in sorted(lines):
        covered_lines = sum(1 for value in lines[name].values() if value > 0)
        total_lines = len(lines[name])
        pct = 100.0 * covered_lines / total_lines if total_lines else 100.0
        if pct < float(minimum):
            low.append((os.path.basename(name), pct))
    if low:
        print()
        print(f"ABAIXO DE {minimum}%:")
        for name, pct in low:
            print(f"  {name}: {pct:.1f}%")
        sys.exit(1)
PY

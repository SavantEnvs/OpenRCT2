#!/usr/bin/env bash
#
# mayhem/test.sh — RUN OpenRCT2's own gtest suite (OpenRCT2Tests, built by mayhem/build.sh with the
# project's normal flags in bin/test/) the way upstream's scripts/run-tests does: index the objects first,
# then run every test. The suite asserts behaviour (S6/park import-export round trips with snapshot
# comparison, ride ratings against golden values, replays checked against recorded checksums, string
# formatting, ini/language parsing, sawyer coding...), so a no-op program fails it.
#
# Excluded (needs data this image does not ship):
#   RideRatings.EverythingPark — the park needs the OpenMusic asset pack's music objects (openrct2.music.*),
#                                a 100+ MB audio download that nothing else here uses.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SRC:=/mayhem}"
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

BIN="$SRC/bin/test"
if [ ! -x "$BIN/OpenRCT2Tests" ] || [ ! -x "$BIN/openrct2-cli" ]; then
  echo "test.sh: $BIN/OpenRCT2Tests or openrct2-cli missing — mayhem/build.sh must build them" >&2
  emit_ctrf gtest 0 1
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/openrct2-test-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
# The suite uses the default user dir ($XDG_CONFIG_HOME/OpenRCT2): give it a private, writable one.
export XDG_CONFIG_HOME="$WORK/config" HOME="$WORK/home"
mkdir -p "$XDG_CONFIG_HOME" "$HOME"

cd "$BIN"
./openrct2-cli scan-objects > "$WORK/scan.log" 2>&1 || { cat "$WORK/scan.log"; }

JSON="$WORK/gtest.json"
./OpenRCT2Tests --gtest_brief=1 --gtest_filter='-RideRatings.EverythingPark' --gtest_output="json:$JSON" \
  > "$WORK/gtest.log" 2>&1
rc=$?
tail -n 40 "$WORK/gtest.log"

if [ ! -s "$JSON" ]; then
  echo "test.sh: OpenRCT2Tests (exit $rc) wrote no results" >&2
  emit_ctrf gtest 0 1
  exit 1
fi

read -r passed failed skipped < <(python3 -I - "$JSON" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
p = f = s = 0
for suite in d.get("testsuites", []):
    for t in suite.get("testsuite", []):
        if t.get("result") == "SKIPPED" or t.get("status") == "NOTRUN":
            s += 1
        elif t.get("failures"):
            f += 1
        else:
            p += 1
print(p, f, s)
PY
)
: "${passed:=0}" "${failed:=1}" "${skipped:=0}"
# A crash/abort mid-run leaves the JSON incomplete or missing tests: count a non-zero exit as a failure.
if [ "$rc" -ne 0 ] && [ "$failed" -eq 0 ]; then failed=1; fi
emit_ctrf gtest "$passed" "$failed" "$skipped"

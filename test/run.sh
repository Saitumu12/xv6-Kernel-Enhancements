#!/usr/bin/env bash
set -u

cd "$(dirname "$0")/.."
mkdir -p test/out

PASS=0
FAIL=0
FAILED_CASES=()

run_case() {
    local name="$1" cpus="$2" boot_to="$3" cmd_to="$4" cmds="$5" required="$6"
    local out="test/out/${name}.log"

    printf '%-28s ' "$name"
    # shellcheck disable=SC2086
    ./test/xv6.exp "$boot_to" "$cmd_to" "$cpus" $cmds > "$out" 2>&1
    local rc=$?

    local problems=()
    if [ $rc -ne 0 ]; then
        problems+=("harness exit $rc")
    fi
    if grep -qE "panic|unexpected trap|kernel page fault" "$out"; then
        problems+=("kernel panic or unexpected trap")
    fi
    if grep -q "FAIL" "$out"; then
        problems+=("test reported FAIL")
    fi
    while IFS= read -r marker; do
        [ -z "$marker" ] && continue
        if ! grep -qF "$marker" "$out"; then
            problems+=("missing: $marker")
        fi
    done <<< "$required"

    if [ ${#problems[@]} -eq 0 ]; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        for p in "${problems[@]}"; do
            echo "    $p"
        done
        echo "    transcript: $out"
        FAIL=$((FAIL + 1))
        FAILED_CASES+=("$name")
    fi
}

WANTED="${1:-all}"

want() {
    [ "$WANTED" = "all" ] || [ "$WANTED" = "$1" ]
}

echo "building"
make -s fs.img xv6.img 2>&1 | grep -vE "^(dd|[0-9]+\+[0-9]+|[0-9]+ bytes)" | tail -5
echo

if want usertests; then
    run_case "usertests-2cpu" 2 120 900 "usertests" "ALL TESTS PASSED"
fi

if want usertests1; then
    run_case "usertests-1cpu" 1 120 900 "usertests" "ALL TESTS PASSED"
fi

if want smoke; then
    run_case "smoke" 2 120 60 "echo\ harness-alive ls" "harness-alive
README"
fi

if want lazy; then
    run_case "lazy" 2 120 300 "lazytest" "lazytest: OK"
fi

if want cow; then
    run_case "cow" 2 120 300 "cowtest" "cowtest: OK"
fi

if want sched; then
    run_case "sched" 2 120 400 "schedtest" "schedtest: OK"
fi

if want sched4; then
    run_case "sched-4cpu" 4 120 400 "schedtest" "schedtest: OK"
fi

if want thread; then
    run_case "thread" 2 120 400 "threadtest" "threadtest: OK"
fi

if want stress; then
    run_case "stress" 4 120 600 "stressfs forktest" "OK"
fi

echo
echo "$PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    echo "failed: ${FAILED_CASES[*]}"
    exit 1
fi

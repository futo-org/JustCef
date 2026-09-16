#!/usr/bin/env bash
# usage: tests/run-all.sh [--strict]
# Every suite is reported as passed, failed or skipped. A skip keeps the exit status at 0
# unless --strict (or JUSTCEF_STRICT=1) is given, which is what CI runs.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${JUSTCEF_TEST_BUILD:-$root/build/tests}
native=${JUSTCEF_NATIVE_PATH:-}
jobs=${JUSTCEF_TEST_JOBS:-4}
suite_timeout=${JUSTCEF_SUITE_TIMEOUT:-900}
sanitizers=${JUSTCEF_SANITIZERS:-"none thread address"}
strict=${JUSTCEF_STRICT:-0}
read -r -a cmake_args <<< "${JUSTCEF_CPP_TESTS_CMAKE_ARGS:-}"
passed=()
failed=()
skipped=()

for arg in ${@+"$@"}; do
    case $arg in
        --strict) strict=1 ;;
        *) echo "usage: $0 [--strict]"; exit 2 ;;
    esac
done

dump_stacks() {
    command -v gdb > /dev/null || return
    [[ -d /proc ]] || return
    for pid in $(pgrep -g "$1" 2>/dev/null); do
        echo "--- stacks of $pid ($(cat /proc/$pid/comm 2>/dev/null))"
        gdb -p "$pid" -batch -ex "thread apply all bt" 2>/dev/null | grep -E "^(Thread|#)"
    done
}

record() {
    if (( $2 == 0 )); then
        passed+=("$1")
        echo "=== $1 passed"
    else
        failed+=("$1")
        echo "=== $1 failed"
    fi
}

skip() {
    skipped+=("$1 ($2)")
    echo "=== $1 skipped: $2"
}

run() {
    local name=$1
    shift
    echo "=== $name"
    if command -v setsid > /dev/null; then
        setsid "$@" &
    else
        "$@" &
    fi
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if (( waited >= suite_timeout )); then
            echo "=== $name timed out after ${suite_timeout}s"
            dump_stacks "$pid"
            kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            failed+=("$name (timeout)")
            return
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    record "$name" $?
}

configure_and_build() {
    local source=$1 dir=$2
    shift 2
    cmake -S "$source" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" > /dev/null && cmake --build "$dir" -j "$jobs"
}

find_header() {
    local file=$1
    shift
    local dir
    for dir in "$@"; do
        if [[ -f $dir/$file ]]; then
            echo "$dir"
            return 0
        fi
    done
    return 1
}

cpp_skip=""
if (( ${#cmake_args[@]} == 0 )); then
    asio_dir=${JUSTCEF_TESTS_ASIO_DIR:-$(find_header asio.hpp "$root/cpp/third_party/asio" "$root/native/third_party/asio" /opt/homebrew/include /usr/local/include /usr/include)}
    json_dir=${JUSTCEF_TESTS_JSON_DIR:-$(find_header json.hpp "$root/cpp/third_party" "$root/native/third_party" /opt/homebrew/include/nlohmann /usr/local/include/nlohmann /usr/include/nlohmann /opt/homebrew/include /usr/local/include /usr/include)}
    if [[ -n $asio_dir && -n $json_dir ]]; then
        cmake_args=(-DJUSTCEF_TESTS_ASIO_DIR="$asio_dir" -DJUSTCEF_TESTS_JSON_DIR="$json_dir")
    else
        cpp_skip="asio.hpp or json.hpp not found, set JUSTCEF_TESTS_ASIO_DIR and JUSTCEF_TESTS_JSON_DIR"
    fi
fi

if [[ -n $native && ! -f $native ]]; then
    native_skip="JUSTCEF_NATIVE_PATH does not exist: $native"
    native=""
elif [[ -z $native ]]; then
    native_skip="JUSTCEF_NATIVE_PATH is not set"
else
    native_skip=""
fi

regenerate_vectors() {
    local dir=$build/protocol-regen
    rm -rf "$dir" && mkdir -p "$dir" || return 1
    cp "$root/tests/protocol/gen_vectors.py" "$root/tests/protocol/vectors.json" "$dir/" || return 1
    python3 "$dir/gen_vectors.py" > /dev/null || return 1
    diff -u "$root/tests/protocol/vectors.json" "$dir/vectors.json" || return 1
}

for san in $sanitizers; do
    dir=$build/ipc-core-$san
    flag=()
    [[ $san != none ]] && flag=(-DJUSTCEF_SANITIZE=$san)
    if configure_and_build "$root/native/tests" "$dir" ${flag[@]+"${flag[@]}"}; then
        run "native ipc core ($san)" "$dir/ipc_core_tests"
    else
        failed+=("native ipc core build ($san)")
    fi
done

fake=$root/tests/fake-native/bin/Debug/net8.0/fake-native
if dotnet build "$root/tests/fake-native/FakeNative.csproj" -v quiet -nologo > /dev/null; then
    run "fake native protocol self-test" "$fake" --self-test "$root/tests/protocol/vectors.json"
    if [[ -n $native ]]; then
        run "C# (real native included)" dotnet test "$root/tests/cs/JustCef.Tests" -nologo
    else
        run "C#" dotnet test "$root/tests/cs/JustCef.Tests" -nologo
        skip "C# real native" "$native_skip"
    fi
else
    failed+=("fake-native build")
fi

echo "=== protocol vectors regenerate"
regenerate_vectors
record "protocol vectors regenerate" $?

if command -v node > /dev/null; then
    run "view renderer lifecycle" node "$root/tests/native/view-renderer.test.cjs"
else
    skip "view renderer lifecycle" "node is not installed"
fi

for san in $sanitizers; do
    if [[ -n $cpp_skip ]]; then
        skip "cpp unit ($san)" "$cpp_skip"
        skip "cpp fake native ($san)" "$cpp_skip"
        continue
    fi
    dir=$build/cpp-$san
    flag=()
    [[ $san != none ]] && flag=(-DJUSTCEF_TESTS_SANITIZER=$san)
    if configure_and_build "$root/tests/cpp" "$dir" ${flag[@]+"${flag[@]}"} ${cmake_args[@]+"${cmake_args[@]}"}; then
        run "cpp unit ($san)" env TSAN_OPTIONS=suppressions=$root/tests/cpp/tsan.supp "$dir/justcef_unit_tests"
        run "cpp fake native ($san)" env TSAN_OPTIONS=suppressions=$root/tests/cpp/tsan.supp "$dir/justcef_fake_tests"
    else
        failed+=("cpp build ($san)")
    fi
done

if [[ -n $native ]]; then
    display=()
    if [[ -z ${DISPLAY:-} && -z ${WAYLAND_DISPLAY:-} ]] && command -v xvfb-run > /dev/null; then
        display=(xvfb-run -a)
    fi
    run "native scenarios" env JUSTCEF_NATIVE="$native" ${display[@]+"${display[@]}"} python3 "$root/tests/native/run.py"
else
    skip "native scenarios" "$native_skip"
fi

echo "=== summary"
(( ${#passed[@]} )) && printf 'passed:  %s\n' ${passed[@]+"${passed[@]}"}
(( ${#skipped[@]} )) && printf 'skipped: %s\n' ${skipped[@]+"${skipped[@]}"}
(( ${#failed[@]} )) && printf 'failed:  %s\n' ${failed[@]+"${failed[@]}"}
printf '%d passed, %d skipped, %d failed\n' "${#passed[@]}" "${#skipped[@]}" "${#failed[@]}"

if (( ${#failed[@]} )); then
    exit 1
fi
if (( ${#skipped[@]} && strict )); then
    echo "strict mode: skipped suites are failures"
    exit 1
fi
exit 0

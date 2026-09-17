#!/usr/bin/env bash
set -euo pipefail

mkdir -p /out/test-logs
trap 'result=$?; if (( result != 0 )); then echo "Container build failed at line $LINENO (exit $result)." >&2; find /out/test-logs -type f -print -exec cat {} \;; fi; exit "$result"' EXIT
case "$TARGETARCH" in
    amd64) architecture=x64 ;;
    arm64) architecture=arm64 ;;
    *) exit 2 ;;
esac
[[ $BUILD_JOBS =~ ^[1-9][0-9]*$ ]]
[[ $RELEASE_VERSION =~ ^[0-9]+$ ]]
{
    echo "revision=$SOURCE_REVISION"
    echo "version=$RELEASE_VERSION"
    echo "architecture=$architecture"
    cmake --version
    c++ --version
    python3 --version
    dpkg-query -W
} > /out/build-info.txt

cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release 2>&1 | tee /out/configure.log
cmake --build native/build --config Release --parallel "$BUILD_JOBS" 2>&1 | tee /out/compile.log
find native/third_party/cef -name '*.tar.bz2' -exec sha256sum {} \; > /out/cef-sha256.txt

while IFS= read -r -d '' binary; do
    if ! readelf -h "$binary" >/dev/null 2>&1; then
        continue
    fi
    readelf --version-info "$binary" > /tmp/elf-versions
    for specification in GLIBC_:2.31 GLIBCXX_:3.4.28; do
        prefix=${specification%%:*}
        ceiling=${specification#*:}
        required=$(grep -oE "${prefix}[0-9.]+" /tmp/elf-versions | sort -Vu | tail -n 1 || true)
        [[ -z $required ]] && continue
        echo "$binary $required" | tee -a /out/abi.txt
        highest=$(printf '%s\n%s\n' "$ceiling" "${required#"$prefix"}" | sort -V | tail -n 1)
        if [[ $highest != "$ceiling" ]]; then
            echo "ABI requirement $required exceeds $prefix$ceiling" >&2
            exit 1
        fi
    done
    strip --strip-unneeded "$binary"
done < <(find native/build/Release -type f -print0)

if [[ $TARGETARCH == arm64 ]]; then
    echo "SKIP browser-runtime verification for ARM64; compilation and ABI checks passed." | tee /out/tests.log
    echo "browser_runtime_tests=not-run-arm64" >> /out/build-info.txt
else
    xvfb-run -a env JUSTCEF_NATIVE=/src/native/build/Release/justcefnative \
        JUSTCEF_TEST_LOGS=/out/test-logs python3 -u tests/native/run.py 2>&1 | tee /out/tests.log
    echo "browser_runtime_tests=passed" >> /out/build-info.txt
fi
cd native/build/Release
zip -r "/out/JustCefNative-linux-$architecture.zip" . > /out/package.log
unzip -tq "/out/JustCefNative-linux-$architecture.zip"
cd /out
sha256sum "JustCefNative-linux-$architecture.zip" > "JustCefNative-linux-$architecture.zip.sha256"

#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
case "${1:-x64}" in
    x64) platform=linux/amd64; architecture=x64 ;;
    arm64) platform=linux/arm64; architecture=arm64 ;;
    *) echo 'Usage: bash native/build-linux.sh [x64|arm64]' >&2; exit 2 ;;
esac
output="$repo_root/build/linux-$architecture"
mkdir -p "$output"
exec > >(tee "$output/build.log") 2>&1
trap 'echo "Linux build failed at line $LINENO (exit $?). See build/linux-'"$architecture"'/build.log." >&2' ERR

docker buildx version
if [[ -z ${BUILDX_BUILDER:-} ]]; then
    export BUILDX_BUILDER=justcef-linux
    if ! docker buildx inspect "$BUILDX_BUILDER" >/dev/null 2>&1; then
        docker buildx create --name "$BUILDX_BUILDER" --driver docker-container \
            --driver-opt image=moby/buildkit:v0.32.2@sha256:28a898719c18a33f4e8000685287fa36fd0dd9560c6440227d3a732d79bb41d8 \
            || docker buildx inspect "$BUILDX_BUILDER"
    fi
fi
docker buildx inspect "$BUILDX_BUILDER" --bootstrap
revision=${CI_COMMIT_SHA:-$(git -C "$repo_root" rev-parse HEAD)}
version=$(sed -n 's/.*JUSTCEF_NATIVE_VERSION=\([0-9][0-9]*\).*/\1/p' "$repo_root/native/src/CMakeLists.txt")
if [[ -n ${CI_COMMIT_TAG:-} && ${CI_COMMIT_TAG#v} != "$version" ]]; then
    echo "Release tag $CI_COMMIT_TAG does not match native version $version." >&2
    exit 1
fi
docker buildx build --platform "$platform" --progress plain \
    --file "$repo_root/native/Dockerfile" \
    --build-arg "SOURCE_REVISION=$revision" \
    --build-arg "RELEASE_VERSION=$version" \
    --build-arg "BUILD_JOBS=${BUILD_JOBS:-2}" \
    --output "type=local,dest=$output" "$repo_root"

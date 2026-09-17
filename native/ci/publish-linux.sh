#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo_root"
case "${1:-}" in
    x64|arm64) architecture=$1 ;;
    *) echo 'Usage: bash native/ci/publish-linux.sh [x64|arm64]' >&2; exit 2 ;;
esac
output="build/linux-$architecture"
mkdir -p "$output"
exec > >(tee "$output/publish.log") 2>&1
trap 'echo "Linux publish failed at line $LINENO (exit $?)." >&2' ERR
: "${CI_COMMIT_TAG:?Publishing requires a release tag}"
: "${CI_COMMIT_SHA:?Publishing requires a pipeline commit}"
: "${CF_R2_ACCOUNT_ID:?Missing R2 account}"
: "${CF_R2_BUCKET:?Missing R2 bucket}"
: "${CF_R2_ACCESS_KEY_ID:?Missing R2 access key}"
: "${CF_R2_SECRET_ACCESS_KEY:?Missing R2 secret key}"
version=${CI_COMMIT_TAG#v}
[[ $version =~ ^[0-9]+$ ]]
grep -Fxq "revision=$CI_COMMIT_SHA" "$output/build-info.txt"
grep -Fxq "version=$version" "$output/build-info.txt"
grep -Fxq "architecture=$architecture" "$output/build-info.txt"
(cd "$output" && sha256sum -c "JustCefNative-linux-$architecture.zip.sha256")
export AWS_ACCESS_KEY_ID=$CF_R2_ACCESS_KEY_ID
export AWS_SECRET_ACCESS_KEY=$CF_R2_SECRET_ACCESS_KEY
export AWS_DEFAULT_REGION=auto
archive="JustCefNative-linux-$architecture.zip"
for filename in "$archive" "$archive.sha256"; do
    docker run --rm \
        -e AWS_ACCESS_KEY_ID -e AWS_SECRET_ACCESS_KEY -e AWS_DEFAULT_REGION \
        -v "$repo_root/$output:/artifacts:ro" \
        public.ecr.aws/aws-cli/aws-cli:2.27.49@sha256:bad3346a39098ab077be6ed58c7e1fe68321a4a844c7c740318100013e6c3581 \
        s3 cp "/artifacts/$filename" "s3://$CF_R2_BUCKET/justcef/$version/$filename" \
        --endpoint-url "https://$CF_R2_ACCOUNT_ID.r2.cloudflarestorage.com" --no-progress
done

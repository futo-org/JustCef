FROM --platform=$BUILDPLATFORM debian:11@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9 AS cef
COPY native/ci/sources.list /etc/apt/sources.list
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl bzip2 && rm -rf /var/lib/apt/lists/*
COPY native/CMakeLists.txt /CEF-CMakeLists.txt
ARG TARGETARCH
RUN set -eu; \
    version=$(sed -n 's/^set(CEF_VERSION "\(.*\)")/\1/p' /CEF-CMakeLists.txt); \
    test -n "$version"; \
    case "$TARGETARCH" in amd64) platform=linux64 ;; arm64) platform=linuxarm64 ;; *) exit 1 ;; esac; \
    archive="cef_binary_${version}_${platform}_minimal.tar.bz2"; \
    mkdir /cef; \
    curl -fL --retry 3 "https://static.grayjay.app/cef/$archive" -o "/cef/$archive"; \
    tar -xjf "/cef/$archive" -C /cef

FROM debian:11@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
COPY native/ci/sources.list /etc/apt/sources.list
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential binutils file ca-certificates curl python3 python-is-python3 \
    libnss3-dev libgtk-3-dev libx11-xcb-dev libxcomposite-dev libxcursor-dev \
    libxi-dev libxtst-dev libxrandr-dev libasound2-dev libxdamage-dev libxss-dev \
    libgbm1 zip unzip xvfb xauth && rm -rf /var/lib/apt/lists/*
ARG TARGETARCH
RUN case "$TARGETARCH" in \
      amd64) arch=x86_64; checksum=83de8839f3fb0d9caf982a0435da3fa8c4fbe2c817dfec99def310dc7e6a8404 ;; \
      arm64) arch=aarch64; checksum=93c3b8920379585dece1314f113c6c9008eaedfe56023c78d856fc86dad5b8e2 ;; \
      *) exit 1 ;; \
    esac && \
    curl -fL --retry 3 "https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-$arch.sh" -o /tmp/cmake.sh && \
    echo "$checksum  /tmp/cmake.sh" | sha256sum -c - && \
    sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm /tmp/cmake.sh

FROM toolchain AS build
WORKDIR /src
COPY native /src/native
COPY --from=cef /cef /src/native/third_party/cef
COPY tests/native /src/tests/native
COPY tests/pages /src/tests/pages
COPY tests/protocol/vectors.json /src/tests/protocol/vectors.json
COPY tests/ci /src/tests/ci
ARG TARGETARCH
ARG SOURCE_REVISION=local
ARG RELEASE_VERSION
ARG BUILD_JOBS=2
RUN python3 tests/ci/test_linux_pipeline.py && bash native/ci/build-linux-container.sh

FROM scratch AS artifacts
COPY --from=build /out/ /

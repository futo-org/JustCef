#/bin/sh
rm -rf build && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-arm64.cmake -DCMAKE_BUILD_TYPE=Release
cd build
make -j8
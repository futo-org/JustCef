#/bin/sh
rm -rf build && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-arm64.cmake -DCMAKE_BUILD_TYPE=Release
cd build
make -j8
/usr/bin/ditto -c -k --sequesterRsrc --keepParent Release/justcefnative.app ../justcefnative-osx-arm64.zip
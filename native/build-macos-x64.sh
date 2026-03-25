#/bin/sh
rm -rf build && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-x64.cmake -DCMAKE_BUILD_TYPE=Release -DPROJECT_ARCH=x86_64
cd build
make -j8
/usr/bin/ditto -c -k --sequesterRsrc --keepParent Release/justcefnative.app ../justcefnative-osx-x64.zip
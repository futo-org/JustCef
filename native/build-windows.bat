IF EXIST build (
    rmdir /s /q build
)

mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
cd Release

powershell Compress-Archive -Path * -DestinationPath "DotCefNative-%PROCESSOR_ARCHITECTURE%.zip"
aws s3 cp "DotCefNative-%PROCESSOR_ARCHITECTURE%.zip" "s3://dotcefnativeartifacts/DotCefNative-%PROCESSOR_ARCHITECTURE%.zip"
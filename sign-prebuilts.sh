#!/bin/bash

./sign-dotcefnative.sh "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-arm64/dotcefnative.app"
./sign-dotcefnative.sh "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-x64/dotcefnative.app"
mv "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-arm64/dotcefnative.app" "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-arm64/dotcefnative.app.orig"
mv "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-x64/dotcefnative.app" "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-x64/dotcefnative.app.orig"
mv "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-arm64/dotcefnative-signed.app" "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-arm64/dotcefnative.app"
mv "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-x64/dotcefnative-signed.app" "/Users/koen/projects/Grayjay.Desktop/JustCef/prebuilt/osx-x64/dotcefnative.app"
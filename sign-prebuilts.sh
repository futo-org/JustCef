#!/bin/bash

./sign-dotcefnative.sh "prebuilt/osx-arm64/dotcefnative.app"
./sign-dotcefnative.sh "prebuilt/osx-x64/dotcefnative.app"
rm -rf "prebuilt/osx-arm64/dotcefnative.app.orig"
rm -rf "prebuilt/osx-x64/dotcefnative.app.orig"
mv "prebuilt/osx-arm64/dotcefnative.app" "prebuilt/osx-arm64/dotcefnative.app.orig"
mv "prebuilt/osx-x64/dotcefnative.app" "prebuilt/osx-x64/dotcefnative.app.orig"
mv "prebuilt/osx-arm64/dotcefnative-signed.app" "prebuilt/osx-arm64/dotcefnative.app"
mv "prebuilt/osx-x64/dotcefnative-signed.app" "prebuilt/osx-x64/dotcefnative.app"
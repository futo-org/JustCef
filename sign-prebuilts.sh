#!/bin/bash

./sign-justcefnative.sh "prebuilt/osx-arm64/justcefnative.app"
./sign-justcefnative.sh "prebuilt/osx-x64/justcefnative.app"
rm -rf "prebuilt/osx-arm64/justcefnative.app.orig"
rm -rf "prebuilt/osx-x64/justcefnative.app.orig"
mv "prebuilt/osx-arm64/justcefnative.app" "prebuilt/osx-arm64/justcefnative.app.orig"
mv "prebuilt/osx-x64/justcefnative.app" "prebuilt/osx-x64/justcefnative.app.orig"
mv "prebuilt/osx-arm64/justcefnative-signed.app" "prebuilt/osx-arm64/justcefnative.app"
mv "prebuilt/osx-x64/justcefnative-signed.app" "prebuilt/osx-x64/justcefnative.app"
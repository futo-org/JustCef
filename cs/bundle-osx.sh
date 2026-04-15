dotnet publish -r osx-x64

rm -rf justceftest.app
mkdir -p justceftest.app
mkdir -p justceftest.app/Contents
mkdir -p justceftest.app/Contents/MacOS
cp Info.plist justceftest.app/Contents/Info.plist

cd bin/Release/net8.0/osx-x64/publish
cp -r `ls . | grep -v '^justcefnative.app$'` ../../../../../justceftest.app/Contents/MacOS
cd ../../../../..

cp -r bin/Release/net8.0/osx-x64/publish/justcefnative.app/Contents/Frameworks justceftest.app/Contents
cp -r bin/Release/net8.0/osx-x64/publish/justcefnative.app/Contents/Resources justceftest.app/Contents
cp -r bin/Release/net8.0/osx-x64/publish/justcefnative.app/Contents/MacOS/justcefnative justceftest.app/Contents/MacOS
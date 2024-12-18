dotnet publish -r osx-x64

rm -rf dotceftest.app
mkdir -p dotceftest.app
mkdir -p dotceftest.app/Contents
mkdir -p dotceftest.app/Contents/MacOS
cp Info.plist dotceftest.app/Contents/Info.plist

cd bin/Release/net8.0/osx-x64/publish
cp -r `ls . | grep -v '^dotcefnative.app$'` ../../../../../dotceftest.app/Contents/MacOS
cd ../../../../..

cp -r bin/Release/net8.0/osx-x64/publish/dotcefnative.app/Contents/Frameworks dotceftest.app/Contents
cp -r bin/Release/net8.0/osx-x64/publish/dotcefnative.app/Contents/Resources dotceftest.app/Contents
cp -r bin/Release/net8.0/osx-x64/publish/dotcefnative.app/Contents/MacOS/dotcefnative dotceftest.app/Contents/MacOS
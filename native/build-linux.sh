#!/bin/sh
sh clean.sh
DOCKER_BUILDKIT=0 docker build -t dotcefnative .
docker create --name dotcefnative dotcefnative
docker cp dotcefnative:/usr/src/app/build/Release ./build-output
docker kill dotcefnative || true
docker rm dotcefnative || true
docker image rm dotcefnative || true
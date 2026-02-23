#!/bin/sh
sh clean.sh
DOCKER_BUILDKIT=0 docker build -t justcefnative .
docker create --name justcefnative justcefnative
docker cp justcefnative:/usr/src/app/build/Release ./build-output
docker kill justcefnative || true
docker rm justcefnative || true
docker image rm justcefnative || true
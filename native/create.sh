#!/bin/sh
DOCKER_BUILDKIT=0 docker build -t dotcefnative .
docker create --name dotcefnative dotcefnative

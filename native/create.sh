#!/bin/sh
DOCKER_BUILDKIT=0 docker build -t justcefnative .
docker create --name justcefnative justcefnative

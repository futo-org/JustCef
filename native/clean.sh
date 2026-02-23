#!/bin/sh
docker kill justcefnative || true
docker rm justcefnative || true
docker image rm justcefnative || true
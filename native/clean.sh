#!/bin/sh
docker kill dotcefnative || true
docker rm dotcefnative || true
docker image rm dotcefnative || true
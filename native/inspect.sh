#!/bin/sh
docker kill dotcefnative
docker rm dotcefnative
docker run --name dotcefnative -it dotcefnative

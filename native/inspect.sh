#!/bin/sh
docker kill justcefnative
docker rm justcefnative
docker run --name justcefnative -it justcefnative

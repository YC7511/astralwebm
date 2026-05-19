ARG ARCH=aarch64
ARG VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS builder

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    jq \
    pkg-config \
    make \
 && rm -rf /var/lib/apt/lists/*

COPY . /opt/app/
WORKDIR /opt/app/app

RUN . /opt/axis/acapsdk/environment-setup* && \
    make clean && \
    make && \
    echo "=== built executable ===" && \
    ls -l /opt/app/app/astralwebm && \
    file /opt/app/app/astralwebm && \
    readelf -h /opt/app/app/astralwebm | sed -n '1,20p'

RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

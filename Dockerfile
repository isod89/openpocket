# OpenPocket build environment: the JieLi toolchain (x86-64 only) + a C compiler, Node and Python for the tests.
# Always linux/amd64 (on Apple silicon and ARM it runs under Docker's emulation): see docs/BUILDING.md.
# The base image is pinned by digest; the toolchain download is pinned by SHA-256 in firmware/setup.sh.
FROM debian:stable-slim@sha256:fbfb4059d2127120aa05b6e6064523d2102552f0cf0026ddf0701c12f1f8f3e0
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential python3 nodejs make curl xz-utils ca-certificates git \
    && rm -rf /var/lib/apt/lists/*
COPY firmware/setup.sh /usr/local/bin/op-setup-toolchain
RUN JIELI_DEST=/opt/jieli sh /usr/local/bin/op-setup-toolchain
# the engine as WebAssembly for the simulated pedal's sound (engine/wasm.sh)
RUN apt-get update && apt-get install -y --no-install-recommends clang lld \
    && rm -rf /var/lib/apt/lists/*
ENV JIELI_TOOLCHAIN=/opt/jieli/toolchain OP_IN_CONTAINER=1
WORKDIR /src

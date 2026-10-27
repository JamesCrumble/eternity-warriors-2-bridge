# Native x86_64 build environment for a static qemu-arm (linux-user). Nothing is installed on the host.
FROM debian:bookworm
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential pkgconf ninja-build python3 python3-venv make xz-utils bzip2 ca-certificates file \
      libglib2.0-dev libpcre2-dev zlib1g-dev libffi-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work

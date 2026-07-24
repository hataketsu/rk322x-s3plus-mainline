# Reproducible cross-build toolchain for RK322x (armv7l) out-of-tree kernel modules.
#
# Host is Apple Silicon (aarch64) under colima/docker; we cross-compile for
# arm-linux-gnueabihf against Armbian kernel headers pinned to a specific version.
#
#   docker build -t rk322x-xbuild \
#     --build-arg KVER=6.6.22-current-rockchip \
#     env/
#
FROM debian:bookworm-slim

ARG KVER=6.6.22-current-rockchip
ENV KVER=${KVER}
ENV ARCH=arm
ENV CROSS_COMPILE=arm-linux-gnueabihf-

RUN dpkg --add-architecture armhf && apt-get update && apt-get install -y --no-install-recommends \
        crossbuild-essential-armhf \
        gcc-arm-linux-gnueabihf \
        bc bison flex libssl-dev libelf-dev kmod \
        build-essential ca-certificates curl wget xz-utils \
        e2fsprogs \
        device-tree-compiler \
        git make \
    && rm -rf /var/lib/apt/lists/*

# Kernel headers are fetched at run time (not baked) so the image stays version-agnostic.
# See env/fetch-headers.sh — it drops a matching KDIR into /kdir.
ENV KDIR=/kdir
WORKDIR /work

COPY fetch-headers.sh /usr/local/bin/fetch-headers
RUN chmod +x /usr/local/bin/fetch-headers

CMD ["bash"]

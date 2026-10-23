# Build environment for build.sh: the GCC that DSperate's committed PGO
# profile was made with (13.3, Ubuntu 24.04 - its CI), cross to aarch64.
# libvulkan-dev is headers only, as in DSperate's CI: without them the core
# compiles without its Vulkan paths and those objects no longer match.
FROM docker.io/library/ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-aarch64-linux-gnu gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    cmake ninja-build git libvulkan-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*

FROM arm32v7/debian:stretch

# Stretch is archived — point to archive.debian.org
# Stretch has glibc 2.24, compatible with Organelle's old Arch Linux ARM
RUN sed -i 's|deb.debian.org|archive.debian.org|g' /etc/apt/sources.list && \
    sed -i '/stretch-updates/d' /etc/apt/sources.list && \
    sed -i 's|security.debian.org|archive.debian.org|g' /etc/apt/sources.list && \
    apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends \
        g++ \
        make \
        libasound2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Usage:
#   docker build --platform linux/arm/v7 -t monosynth-builder CppMonoSynth/
#   docker run --platform linux/arm/v7 --rm -v $(pwd)/CppMonoSynth:/build monosynth-builder make

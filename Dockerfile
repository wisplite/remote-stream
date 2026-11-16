FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libssl-dev \
    libxcb-damage0-dev \
    libxcb-keysyms1-dev \
    libxcb-shm0-dev \
    libxcb-xfixes0-dev \
    libxcb-xtest0-dev \
    libxcb1-dev \
    nasm \
    ninja-build \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY web ./web

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTATIC_BUILD=ON \
    && cmake --build build

FROM scratch AS export
COPY --from=builder /src/build/remote_stream /remote_stream

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libhiredis-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j "$(nproc)"

CMD ["build/lob-engine"]

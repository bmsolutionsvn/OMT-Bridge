# syntax=docker/dockerfile:1

FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config libsrt-openssl-dev doctest-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY remote /src/remote
RUN cmake -S /src/remote -B /b -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /b -j"$(nproc)" \
    && /b/remote_tests

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsrt1.5-openssl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /b/bmb-relay /b/srt-bench /usr/local/bin/
COPY remote/docker/selftest.sh /usr/local/bin/selftest.sh
RUN chmod +x /usr/local/bin/selftest.sh
EXPOSE 7000/udp
ENTRYPOINT ["bmb-relay"]
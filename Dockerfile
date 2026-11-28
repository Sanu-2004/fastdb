ARG DEBIAN_BASE=debian:bookworm-slim@sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e
ARG FASTDB_ENABLE_LZ4=OFF
ARG FASTDB_ENABLE_ZSTD=OFF
FROM ${DEBIAN_BASE} AS build
ARG FASTDB_ENABLE_LZ4
ARG FASTDB_ENABLE_ZSTD
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake gcc libc6-dev make \
    && if [ "${FASTDB_ENABLE_LZ4}" = "ON" ]; then apt-get install -y --no-install-recommends liblz4-dev pkg-config; fi \
    && if [ "${FASTDB_ENABLE_ZSTD}" = "ON" ]; then apt-get install -y --no-install-recommends libzstd-dev; fi \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY cli/ cli/
COPY include/ include/
COPY src/core/ src/core/
COPY src/storage/ src/storage/
COPY tests/ tests/
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DFASTDB_ENABLE_LZ4="${FASTDB_ENABLE_LZ4}" \
        -DFASTDB_ENABLE_ZSTD="${FASTDB_ENABLE_ZSTD}" \
        -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib \
    && cmake --build /build --parallel \
    && ctest --test-dir /build --output-on-failure \
    && DESTDIR=/stage cmake --install /build

FROM ${DEBIAN_BASE} AS runtime
ARG FASTDB_ENABLE_LZ4
RUN if [ "${FASTDB_ENABLE_LZ4}" = "ON" ]; then \
        apt-get update && apt-get install -y --no-install-recommends liblz4-1 \
        && rm -rf /var/lib/apt/lists/*; \
    fi
RUN groupadd --gid 10001 fastdb \
    && useradd --uid 10001 --gid 10001 --home-dir /data --no-create-home \
        --no-log-init --shell /usr/sbin/nologin fastdb \
    && install -d -m 0700 -o fastdb -g fastdb /data
COPY --from=build /stage/usr/local/ /usr/local/
RUN ldconfig
USER 10001:10001
WORKDIR /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/fastdb-cli"]
CMD ["--lsm", "/data"]

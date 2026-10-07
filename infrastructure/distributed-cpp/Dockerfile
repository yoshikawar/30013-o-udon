FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY bridge/ bridge/
RUN cmake -S bridge -B /build/bridge -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build/bridge --parallel 2
ARG APP_DIR=apps/example
COPY ${APP_DIR}/ app/
RUN cmake -S app -B /build/app -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build/app --parallel 2 \
    && cmake --install /build/app --prefix /opt/application

FROM debian:bookworm-slim AS runtime-common
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 ca-certificates && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home runner \
    && mkdir /state && chown runner:runner /state
COPY --from=build /opt/application/ /opt/application/
USER runner
WORKDIR /state

FROM runtime-common AS main
COPY --from=build /build/bridge/cluster_main /usr/local/bin/cluster_main
ENTRYPOINT ["/usr/local/bin/cluster_main"]

FROM runtime-common AS worker
COPY --from=build /build/bridge/cluster_worker /usr/local/bin/cluster_worker
ENTRYPOINT ["/usr/local/bin/cluster_worker"]

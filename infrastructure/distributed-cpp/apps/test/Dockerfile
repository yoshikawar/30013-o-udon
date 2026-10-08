FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ libcurl4-openssl-dev make nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ARG SOURCE_REVISION
RUN test -n "${SOURCE_REVISION}" \
    && cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DHEXA_UDON_BUILD_FINGERPRINT_OVERRIDE="${SOURCE_REVISION}" \
    && cmake --build /build --parallel 2 \
    && cmake --install /build --prefix /opt/hexa-udon

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libcurl4 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home runner \
    && mkdir /state && chown runner:runner /state
COPY --from=build /opt/hexa-udon/bin/hexa_udon /usr/local/bin/hexa_udon
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
USER runner
WORKDIR /state
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]

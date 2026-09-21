# Reproducible Linux runtime for scored Quidra benchmark work.
#
# Two stages on purpose:
#
#   base        OS, Python and the native dependencies needed to build the Quidra
#               compiler from the evaluated snapshot. Small enough that CI can
#               build it on every change and still exercise the real launcher,
#               the real gateway socket and a real `preflight`.
#   toolchains  base plus the nine pinned comparison-language toolchains. This is
#               the image a measurement run uses.
#
# The Quidra compiler is never baked in. It is built during the run from
# /quidra-benchmark/repo, so the image cannot pin the thing under evaluation.

FROM ubuntu:24.04 AS base

ARG CLANG_MAJOR=18

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=UTC \
    PYTHONDONTWRITEBYTECODE=1

# The native dependency list matches the repository's own Linux CI so the
# evaluated snapshot builds here exactly as it does there.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip xz-utils tar git make \
      python3 python3-venv \
      build-essential pkg-config ninja-build cmake \
      clang-${CLANG_MAJOR} lld-${CLANG_MAJOR} llvm-${CLANG_MAJOR}-dev llvm-${CLANG_MAJOR} \
      libcurl4-openssl-dev libpng-dev libjpeg-dev libtiff-dev libwebp-dev \
      ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
      zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# The benchmark fingerprints C++ with `c++ --version` while `micro_measure.py`
# compiles with `clang++`. Repointing the distribution alternatives makes those
# the same compiler, so the recorded fingerprint describes what actually ran.
RUN update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-${CLANG_MAJOR} 100 \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/clang-${CLANG_MAJOR} 100 \
 && update-alternatives --set c++ /usr/bin/clang++-${CLANG_MAJOR} \
 && update-alternatives --set cc /usr/bin/clang-${CLANG_MAJOR} \
 && ln -sf /usr/bin/clang++-${CLANG_MAJOR} /usr/bin/clang++ \
 && ln -sf /usr/bin/clang-${CLANG_MAJOR} /usr/bin/clang

# The sandbox always runs unprivileged. The account exists so an interactive
# `docker run` without --user is still non-root; the launcher additionally pins
# --user explicitly so the bind-mounted staging directory stays writable.
# ubuntu:24.04 ships its own uid-1000 account, so that uid has to be freed
# before this one can claim it. The guard keeps the build working on a future
# base image that drops the default account.
RUN userdel --remove ubuntu >/dev/null 2>&1 || true; \
    useradd --create-home --uid 1000 --shell /usr/sbin/nologin benchmark \
 && mkdir -p /quidra-benchmark /quidra-benchmark/gateway /opt/quidra-benchmark \
 && chown -R 1000:1000 /quidra-benchmark /opt/quidra-benchmark

ENV HOME=/quidra-benchmark/home \
    TMPDIR=/quidra-benchmark/tmp \
    PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin

WORKDIR /quidra-benchmark
USER 1000:1000
CMD ["python3", "/quidra-benchmark/template/scripts/benchmark.py", "preflight"]


FROM base AS toolchains

USER root

ARG CLANG_MAJOR=18
ARG RUST_PIN
ARG GO_PIN
ARG JAVA_PIN
ARG JAVA_BUILD
ARG KOTLIN_PIN
ARG NODE_PIN
ARG TYPESCRIPT_PIN
ARG SWIFT_PIN
ARG ZIG_PIN

# Swift's Linux runtime needs a few shared libraries beyond the base set.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libncurses6 libpython3-dev libsqlite3-0 libxml2 tzdata \
    && rm -rf /var/lib/apt/lists/*

COPY install_toolchains.sh /opt/quidra-benchmark/install_toolchains.sh
RUN chmod +x /opt/quidra-benchmark/install_toolchains.sh \
 && /opt/quidra-benchmark/install_toolchains.sh

COPY toolchains.json verify_toolchains.py /opt/quidra-benchmark/runtime/
RUN python3 /opt/quidra-benchmark/runtime/verify_toolchains.py \
 && chown -R 1000:1000 /opt/quidra-benchmark

ENV JAVA_HOME=/opt/java \
    GOTOOLCHAIN=local \
    RUSTUP_HOME=/opt/rust \
    CARGO_HOME=/opt/rust

WORKDIR /quidra-benchmark
USER 1000:1000
CMD ["python3", "/quidra-benchmark/template/scripts/benchmark.py", "preflight"]

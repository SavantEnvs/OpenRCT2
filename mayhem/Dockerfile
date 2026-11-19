# syntax=docker/dockerfile:1.7
#
# OpenRCT2 commit-image Dockerfile — builds ON TOP of the org C/C++ base
# image (ghcr.io/savantenvs/base). The base already provides debian + clang/llvm +
# libFuzzer/afl++ + the identities + the build-contract ENV, so this only adds the
# per-commit build (built on top of the prebuilt base, rather than building the toolchain in-image).
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) \
#       [--build-arg SOURCE_DATE_EPOCH=$(date +%s)] \
#       -t mytag .

# The org base, pinned to a fixed DATE-tagged base (YYYYMMDD; shared by all integrations
# minted against it). Use the current dated tag; pin by digest in production: ...base@sha256:<digest>.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang ("must be a non-negative decimal integer") both here and for
# any in-image rebuild. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH

# Sanitizer override: the base exports SANITIZER_FLAGS (ASan+UBSan, halting) as the default.
# To change it for a build (e.g. ASan-only, or add MSan): --build-arg SANITIZER_FLAGS="...".
# Declare it WITHOUT a default (no '='): an undefaulted ARG lets the base ENV flow through to
# build.sh when not passed, and an explicit --build-arg overrides it. (An empty default — `=` —
# would instead shadow the base ENV and silently wipe the sanitizers.)
ARG SANITIZER_FLAGS
# Coverage (optional, off by default): instrument the TEST build to measure how much of the project
# the suite exercises — --build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping".
# Empty by default (no effect on the normal oracle build); build.sh appends it to the test build only.
ARG COVERAGE_FLAGS=
# Build parallelism (optional). build.sh falls back to nproc when unset: --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

# Build dependencies of the headless libopenrct2 (+ GoogleTest for upstream's suite), pinned through
# SOURCE_DATE_EPOCH when it is set.
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
      libzip-dev libpng-dev libzstd-dev libicu-dev nlohmann-json3-dev libgtest-dev \
 && rm -rf /var/lib/apt/lists/*

# Runtime data upstream's `install` step downloads (assets.json): the OpenRCT2 object set every park,
# object, replay and simulate target loads against, and the replay set upstream's ReplayTests play back.
# Pinned by version + sha256 and unpacked read-only under /opt/toolchains/deps (SPEC §6.2 item 19);
# build.sh symlinks them into the (gitignored) data/object and test/tests/testdata/replays.
ARG OBJECTS_VER=1.8.1
ARG OBJECTS_SHA256=3252a55634db940ebca1df6c8e1f34eb53784d77d8ed99d668f4613196540c4f
ARG REPLAYS_VER=0.0.98
ARG REPLAYS_SHA256=8c64f3105e54969b6dda2d93835583975c76f684a806c6912e40b106d291b69a
RUN set -e; \
    fetch() { d="/opt/toolchains/deps/$1"; mkdir -p "$d/src"; curl -fsSL -o /tmp/dep.zip "$2"; \
              echo "$3  /tmp/dep.zip" | sha256sum -c -; unzip -q /tmp/dep.zip -d "$d/src"; rm -f /tmp/dep.zip; \
              chmod -R a+rX,go-w "$d"; }; \
    fetch "openrct2-objects-${OBJECTS_VER}" \
      "https://github.com/OpenRCT2/objects/releases/download/v${OBJECTS_VER}/objects.zip" "${OBJECTS_SHA256}"; \
    fetch "openrct2-replays-${REPLAYS_VER}" \
      "https://github.com/OpenRCT2/replays/releases/download/v${REPLAYS_VER}/replays.zip" "${REPLAYS_SHA256}"
USER mayhem

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git
# history) is copied to /mayhem as `mayhem`; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Anything a Dockerfile step materializes into the tree (downloaded fixtures, generated headers,
# vendored deps) MUST be committed or (re)created by build.sh: rlenv's patch grader runs
# `git clean -ffdX` before every graded build, so a gitignored, untracked file is gone by then and
# every honest patch scores zero (SPEC §6.2 item 16; verify-repo.sh checks it).
#
# OPTIONAL compile-cache pre-fill (SPEC §6.2 item 18). Bases minted with /opt/mayhem/ccache/prefill
# run the build under rlenv's pinned ccache, so the patch grader's first (otherwise cold) rebuild is
# warm — this is what keeps large C/C++ targets inside the build time limit. Older bases fall back to
# the plain build; a missing or stale pre-fill only costs a cold cache. Never enable ccache in
# build.sh or via ENV/PATH, and keep the compiler resolving by name (CC=clang) or nothing is cached.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem

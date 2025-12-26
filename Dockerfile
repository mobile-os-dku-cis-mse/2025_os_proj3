
FROM gcc:13-bookworm AS build

WORKDIR /build

# =========================================================================
# Stage 1: Strategic COPY to Optimize Docker Layer Caching
# -------------------------------------------------------------------------
# Files are copied in order of increasing volatility (change frequency).
# This minimizes cache invalidation and drastically speeds up rebuilds.
# =========================================================================

# 1. Copy build configuration first
COPY Makefile .
# 2. Sync interfaces (headers)
COPY include/ include/
# 3. Sync implementation (source) last
COPY src/ src/

# Compile the binary
RUN make

# =========================================================================
# Stage 2: Analysis & Debug Stage (Observation Layer)
# -------------------------------------------------------------------------
# This stage provides an environment to verify the storage layer's 
# behavior against the Linux kernel using low-level tracing tools.
# =========================================================================
FROM debian:bookworm-slim AS debug

# Install tracing and debugging tools
RUN apt-get update && apt-get install -y \
    strace \
    gdb \
    procps \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /build/simplefs /app/simplefs

# [Security/Capability Note]
# We retain ROOT privileges in this stage specifically to enable 
# 'SYS_PTRACE' capabilities required for strace and kernel-level debugging.
ENTRYPOINT ["/bin/bash"]

# =========================================================================
# Stage 3: Runtime Stage (Hardened Production Environment)
# -------------------------------------------------------------------------
# This stage focuses on minimizing the attack surface and optimizing 
# I/O performance for a production-ready storage engine.
# =========================================================================
FROM debian:bookworm-slim AS release

# 1. Principle of Least Privilege: running as a non-root user.
RUN useradd -m -u 10001 simplefs
WORKDIR /app

# 2. Minimal Artifact Deployment: only the compiled binary is extracted.
COPY --from=build /build/simplefs /app/simplefs

# 3. I/O Optimization (Storage Performance): bypass the copy-on-write overhead of OverlayFS
VOLUME ["/data"]

# 4. Runtime Security Hardening
USER simplefs
RUN chmod 0555 /app/simplefs

# 5. Predictable Execution
ENTRYPOINT ["/app/simplefs"]

# FROM gcc:13-bookworm

# RUN apt-get update && apt-get install -y \
#     gdb \
#     strace \
#     procps \
#     make \
#     && rm -rf /var/lib/apt/lists/*

# WORKDIR /work

# ENTRYPOINT ["/bin/bash"]

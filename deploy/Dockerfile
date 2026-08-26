# ---- Stage 1: builder ----
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make libprotobuf-dev protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)

# ---- Stage 2: runtime ----
FROM ubuntu:24.04

# 仅安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    libprotobuf32t64 curl \
    && rm -rf /var/lib/apt/lists/*

# 只复制需要的二进制文件
RUN mkdir -p /app/registryd /app/examples
COPY --from=builder /src/build/registryd/registryd /app/registryd/
COPY --from=builder /src/build/examples/math_server /app/examples/

WORKDIR /app

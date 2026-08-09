# Stage 1: Build stage
FROM gcc:14 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    make \
    libpq-dev \
    libsodium-dev \
    libssl-dev \
    libhiredis-dev \
    nlohmann-json3-dev \
    protobuf-compiler \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    librdkafka-dev \
    librabbitmq-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code and Makefile
COPY Makefile ./
COPY protos/ ./protos/
COPY src/ ./src/
COPY include/ ./include/

# Generate protobuf stubs
RUN mkdir -p include/notenest/proto src/proto && \
    protoc -Iprotos --cpp_out=src/proto protos/*.proto && \
    protoc -Iprotos --grpc_out=src/proto --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) protos/*.proto

# Build the application
RUN make -j$(nproc)

# Stage 2: Runtime stage
FROM debian:trixie-slim AS runner

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libpq5 \
    libsodium23 \
    libssl3 \
    libhiredis1.1.0 \
    libgrpc++-dev \
    libprotobuf-dev \
    librdkafka-dev \
    librabbitmq-dev \
    postgresql-client \
    curl \
    && rm -rf /var/lib/apt/lists/*


# Set working directory
WORKDIR /app

# Copy the compiled binary from the builder stage
COPY --from=builder /app/notenest ./notenest
COPY db/ ./db/
COPY entrypoint.sh ./entrypoint.sh
RUN chmod +x entrypoint.sh ./db/migrate.sh

# Expose ports
EXPOSE 8080 50053 50054

# Entrypoint script executes DB migrations before starting application
ENTRYPOINT ["./entrypoint.sh"]

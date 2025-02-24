# syntax=docker/dockerfile:1.4
# Stage 1: Builder
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y \
    ansible-core \
    build-essential \
    cmake \
    curl \
    git \
    ninja-build \
    lsb-release \
    openssh-client \
    pkg-config \
    zip \
    tar \
    unzip

COPY ansible/fast.yml /tmp/fast.yml

# Run the playbook using the "local" connection.
RUN ansible-playbook -i "localhost," -c local --extra-vars "home_dir=/tmp/fast-build" /tmp/fast.yml

# Stage 2: Runtime image
FROM debian:bookworm-slim

# RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6

COPY --from=builder /tmp/fast-build /root/

WORKDIR "/root"

CMD ["/bin/bash"]

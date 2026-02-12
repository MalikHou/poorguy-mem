FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PGMEM_PYTHON_BIN=python3.11

RUN apt-get update && apt-get install -y --no-install-recommends \
    apt-utils \
    bash \
    ca-certificates \
    curl \
    git \
    jq \
    gnupg \
    openssh-client \
    pkg-config \
    procps \
    software-properties-common \
    strace \
    vim \
    wget \
    sudo \
    clangd \
    build-essential \
    cmake \
    make \
    ninja-build \
    net-tools \
    libboost-context-dev \
    libboost-dev \
    libcurl4-openssl-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libjsoncpp-dev \
    libprotobuf-dev \
    libprotoc-dev \
    libssl-dev \
    liburing-dev \
    libzstd-dev \
    protobuf-compiler \
    zlib1g-dev \
 && add-apt-repository -y ppa:deadsnakes/ppa \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
    python3.11 \
    python3.11-dev \
    python3.11-venv \
    python3.11-distutils \
 && ln -sf /usr/bin/python3.11 /usr/local/bin/python-stable \
 && rm -rf /var/lib/apt/lists/*

USER root

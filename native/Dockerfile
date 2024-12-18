FROM debian:11
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    curl \
    pkg-config \
    libssl-dev \
    zlib1g-dev \
    libncurses5-dev \
    libgtk-3-dev \ 
    libncursesw5-dev \
    libreadline-dev \
    libsqlite3-dev \
    libffi-dev \
    libbz2-dev \
    liblzma-dev \
    jq \
    clang-18 \
    clang++-18
RUN rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-18 100
RUN update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-18 100

RUN wget https://www.python.org/ftp/python/3.11.10/Python-3.11.10.tgz && \
    tar -xvzf Python-3.11.10.tgz && \
    cd Python-3.11.10 && \
    ./configure --enable-optimizations && \
    make -j$(nproc) && \
    make install && \
    cd .. && \
    rm -rf Python-3.11.10 Python-3.11.10.tgz

RUN python3.11 --version && python3.11 -m pip install --upgrade pip && \
    python3.11 -m pip install six

RUN ln -sf /usr/local/bin/python3.11 /usr/bin/python && \
    ln -sf /usr/local/bin/pip3.11 /usr/bin/pip

WORKDIR /usr/src/app
COPY cmake ./cmake
COPY scripts ./scripts
COPY src ./src
COPY tools ./tools
COPY third_party ./third_party
COPY CMakeLists.txt CMakeLists.txt

RUN mkdir -p build
WORKDIR /usr/src/app/build
RUN cmake -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 ..
RUN cmake --build .
ENTRYPOINT ["/bin/bash"]

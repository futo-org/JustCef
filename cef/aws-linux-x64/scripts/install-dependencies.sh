#!/bin/sh
sudo apt-get update
# Install essential build dependencies for building Python from source
sudo apt-get install -y \
    git \
    curl \
    file \
    lsb-release \
    procps \
    wget \
    build-essential \
    zlib1g-dev \
    libncurses5-dev \
    libgdbm-dev \
    libnss3-dev \
    libssl-dev \
    libreadline-dev \
    libffi-dev \
    libbz2-dev \
    libsqlite3-dev \
    uuid-dev \
    tk-dev

sudo -u ubuntu -H git config --global core.autocrlf false
sudo -u ubuntu -H git config --global core.filemode false
sudo -u ubuntu -H git config --global core.fscache true
sudo -u ubuntu -H git config --global core.preloadindex true

wget https://www.python.org/ftp/python/3.11.10/Python-3.11.10.tgz
tar -xvzf Python-3.11.10.tgz
cd Python-3.11.10
./configure --enable-optimizations
make -j$(nproc)
sudo make install
cd ..
rm -rf Python-3.11.10 Python-3.11.10.tgz
sudo ln -sf /usr/local/bin/python3.11 /usr/bin/python
sudo ln -sf /usr/local/bin/pip3.11 /usr/bin/pip
python3.11 --version && python3.11 -m pip install --upgrade pip
curl 'https://chromium.googlesource.com/chromium/src/+/main/build/install-build-deps.py?format=TEXT' | base64 -d > install-build-deps.py
python3 ./install-build-deps.py --no-chromeos-fonts --no-nacl
python3 -m pip install dataclasses importlib_metadata
sudo mkdir -p /home/ubuntu/code
sudo chown ubuntu:ubuntu /home/ubuntu/code
sudo chmod 755 /home/ubuntu/code

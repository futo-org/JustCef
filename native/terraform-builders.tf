provider "aws" {
  region = "us-east-1"  # Virginia
}

data "aws_vpc" "default" {
  default = true
}

#resource "aws_security_group" "build_server_dotcefnative_sg" {
#  name        = "build_server_dotcefnative_security_group"
#  description = "Security group for build server allowing SSH access"
#  vpc_id      = data.aws_vpc.default.id
#
#  ingress {
#    description = "Allow SSH access"
#    from_port   = 22
#    to_port     = 22
#    protocol    = "tcp"
#    cidr_blocks = ["0.0.0.0/0"]
#  }
#
#  egress {
#    from_port   = 0
#    to_port     = 0
#    protocol    = "-1"
#    cidr_blocks = ["0.0.0.0/0"]
#  }
#}

# x64 Build Server
resource "aws_instance" "build_server_dotcefnative_x64" {
  ami                    = "ami-0057500c2b673caf5"  # Debian 11 AMI x64
  instance_type          = "t3.micro"
  key_name               = "koen@pop-os"
  iam_instance_profile   = "EC2_Secret_ReadWrite"
  security_groups        = ["build_server_dotcefnative_security_group"]
  instance_initiated_shutdown_behavior = "terminate"

  root_block_device {
    volume_size = 32
    volume_type = "gp3"
    delete_on_termination = true
  }

  user_data = <<-EOF
    #!/bin/bash
    fallocate -l 2G /swapfile
    chmod 600 /swapfile
    mkswap /swapfile
    swapon /swapfile

    # Update and install dependencies
    apt update
    apt install -y git git-lfs build-essential zlib1g-dev libnss3-dev libatk1.0-dev \
                  libx11-xcb-dev libxcomposite-dev libxcursor-dev libxi-dev \
                  libxtst-dev libxrandr-dev libasound2-dev libxdamage-dev \
                  libxss-dev libglib2.0-dev libnss3 libgtk-3-dev curl \
                  libssl-dev ca-certificates zip

    # Configure Git LFS
    git lfs install

    # Python installation as per user example
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

    # Install CMake
    CMAKE_VERSION=3.30.5
    wget https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.sh
    chmod +x cmake-$CMAKE_VERSION-linux-x86_64.sh
    ./cmake-$CMAKE_VERSION-linux-x86_64.sh --skip-license --prefix=/usr/local
    rm cmake-$CMAKE_VERSION-linux-x86_64.sh
    cmake --version

    # Set up SSH keys for Git
    PRIVATE_KEY=$(aws secretsmanager get-secret-value --secret-id "updater/PrivateKey" --query SecretString --output text --region us-east-1)
    echo "$PRIVATE_KEY" > /root/.ssh/id_rsa
    chmod 600 /root/.ssh/id_rsa
    PUBLIC_KEY=$(aws secretsmanager get-secret-value --secret-id "updater/PublicKey" --query SecretString --output text --region us-east-1)
    echo "$PUBLIC_KEY" > /root/.ssh/id_rsa.pub
    chmod 600 /root/.ssh/id_rsa.pub

    # Clone the CEF project
    cd /root
    ssh-keyscan -H gitlab.futo.org >> ~/.ssh/known_hosts
    git clone git@gitlab.futo.org:videostreaming/JustCef.git
    cd JustCef
    git lfs pull

    # Navigate to native directory and set up CMake build
    cd /root/JustCef/native
    mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    cmake --build . --config Release

    # Compress the output and upload to S3
    cd Release
    zip -r DotCefNative-linux-x64.zip *
    aws s3 cp DotCefNative-linux-x64.zip s3://dotcefnativeartifacts/DotCefNative-linux-x64.zip

    shutdown -h now
  EOF

  tags = {
    Name = "debian-build-server-x64"
  }
}

# ARM64 Build Server
resource "aws_instance" "build_server_dotcefnative_arm64" {
  ami                    = "ami-01476ceec1d5e9a72"  # Debian 11 AMI ARM64
  instance_type          = "t4g.micro"
  key_name               = "koen@pop-os"
  iam_instance_profile   = "EC2_Secret_ReadWrite"
  security_groups        = ["build_server_dotcefnative_security_group"]
  instance_initiated_shutdown_behavior = "terminate"

  root_block_device {
    volume_size = 32
    volume_type = "gp3"
    delete_on_termination = true
  }

  user_data = <<-EOF
    #!/bin/bash
    fallocate -l 2G /swapfile
    chmod 600 /swapfile
    mkswap /swapfile
    swapon /swapfile

    # Update and install dependencies
    apt update
    apt install -y git git-lfs build-essential zlib1g-dev libnss3-dev libatk1.0-dev \
                  libx11-xcb-dev libxcomposite-dev libxcursor-dev libxi-dev \
                  libxtst-dev libxrandr-dev libasound2-dev libxdamage-dev \
                  libxss-dev libglib2.0-dev libnss3 libgtk-3-dev curl \
                  libssl-dev ca-certificates zip

    # Configure Git LFS
    git lfs install

    # Python installation as per user example
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

    # Install CMake
    CMAKE_VERSION=3.30.5
    wget https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-aarch64.sh
    chmod +x cmake-$CMAKE_VERSION-linux-aarch64.sh
    ./cmake-$CMAKE_VERSION-linux-aarch64.sh --skip-license --prefix=/usr/local
    rm cmake-$CMAKE_VERSION-linux-aarch64.sh
    cmake --version

    # Set up SSH keys for Git
    PRIVATE_KEY=$(aws secretsmanager get-secret-value --secret-id "updater/PrivateKey" --query SecretString --output text --region us-east-1)
    echo "$PRIVATE_KEY" > /root/.ssh/id_rsa
    chmod 600 /root/.ssh/id_rsa
    PUBLIC_KEY=$(aws secretsmanager get-secret-value --secret-id "updater/PublicKey" --query SecretString --output text --region us-east-1)
    echo "$PUBLIC_KEY" > /root/.ssh/id_rsa.pub
    chmod 600 /root/.ssh/id_rsa.pub

    # Clone the CEF project
    cd /root
    ssh-keyscan -H gitlab.futo.org >> ~/.ssh/known_hosts
    git clone git@gitlab.futo.org:videostreaming/JustCef.git
    cd JustCef
    git lfs pull

    # Navigate to native directory and set up CMake build
    cd /root/JustCef/native
    mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    cmake --build . --config Release

    # Compress the output and upload to S3
    cd Release
    zip -r DotCefNative-linux-arm64.zip *
    aws s3 cp DotCefNative-linux-arm64.zip s3://dotcefnativeartifacts/DotCefNative-linux-arm64.zip

    shutdown -h now
  EOF

  tags = {
    Name = "debian-build-server-arm64"
  }
}

output "public_ip_x64" {
  value = aws_instance.build_server_dotcefnative_x64.public_ip
}

output "public_ip_arm64" {
  value = aws_instance.build_server_dotcefnative_arm64.public_ip
}
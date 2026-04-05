terraform {
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = "us-east-1"
}

##########################
# Security Group
##########################

resource "aws_security_group" "ssh" {
  name        = "cef-builder-linux-x64-sg"
  description = "Allow SSH inbound traffic"

  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  egress {
    description = "Allow all outbound traffic"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

##########################
# EC2 Instance and Provisioning
##########################

resource "aws_instance" "ubuntu" {
  ami                    = "ami-04b4f1a9cf54c11d0"
  instance_type          = "c5.2xlarge"
  key_name               = "koen@pop-os"
  vpc_security_group_ids = [aws_security_group.ssh.id]

  tags = {
    Name = "cef-builder-linux-x64"
  }

  root_block_device {
    volume_size           = 256
    volume_type           = "gp3"
    delete_on_termination = true
  }

  connection {
    type = "ssh"
    user = "ubuntu"
    host = self.public_ip
  }

  provisioner "remote-exec" {
    inline = [
      "mkdir -p /home/ubuntu/code/cefbuilder/automate",
      "mkdir -p /home/ubuntu/code/cefbuilder/patches/cef",
      "mkdir -p /home/ubuntu/code/cefbuilder/aws-linux-x64/scripts"
    ]
  }

  provisioner "file" {
    source      = "scripts/create-swap.sh"
    destination = "/tmp/create-swap.sh"
  }

  provisioner "file" {
    source      = "scripts/disable-updates.sh"
    destination = "/tmp/disable-updates.sh"
  }

  provisioner "file" {
    source      = "scripts/install-dependencies.sh"
    destination = "/tmp/install-dependencies.sh"
  }

  provisioner "file" {
    source      = "scripts/clone-repos.sh"
    destination = "/tmp/clone-repos.sh"
  }

  provisioner "file" {
    source      = "../cef.branch"
    destination = "/home/ubuntu/code/cefbuilder/cef.branch"
  }

  provisioner "file" {
    source      = "../automate/automate-git.py"
    destination = "/home/ubuntu/code/cefbuilder/automate/automate-git.py"
  }

  provisioner "file" {
    source      = "../patches/apply_cef_patches.py"
    destination = "/home/ubuntu/code/cefbuilder/patches/apply_cef_patches.py"
  }

  provisioner "file" {
    source      = "../patches/cef/0001-file-paths.patch"
    destination = "/home/ubuntu/code/cefbuilder/patches/cef/0001-file-paths.patch"
  }

  provisioner "file" {
    source      = "scripts/build-x64.sh"
    destination = "/home/ubuntu/code/cefbuilder/aws-linux-x64/scripts/build-x64.sh"
  }

  provisioner "file" {
    source      = "scripts/build.sh"
    destination = "/home/ubuntu/code/cefbuilder/aws-linux-x64/scripts/build.sh"
  }

  provisioner "file" {
    source      = "scripts/build-arm64.sh"
    destination = "/home/ubuntu/code/cefbuilder/aws-linux-x64/scripts/build-arm64.sh"
  }

  provisioner "remote-exec" {
    inline = [
      "chmod +x /tmp/*.sh",
      "chmod +x /home/ubuntu/code/cefbuilder/aws-linux-x64/scripts/*.sh",
      "sudo /tmp/create-swap.sh",
      "sudo /tmp/disable-updates.sh",
      "sudo /tmp/install-dependencies.sh",
      "sudo /tmp/clone-repos.sh"
    ]
  }
}

##########################
# Outputs
##########################
output "private_ip" {
  description = "Private IP of the Windows instance."
  value       = aws_instance.ubuntu.private_ip
}

output "public_ip" {
  description = "Public IP of the Windows instance."
  value       = aws_instance.ubuntu.public_ip
}

output "instance_id" {
  description = "ID of the Windows instance."
  value       = aws_instance.ubuntu.id
}

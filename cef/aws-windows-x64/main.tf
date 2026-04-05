terraform {
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
    random = {
      source  = "hashicorp/random"
      version = "~> 3.1"
    }
  }
}

##########################
# Provider
##########################
provider "aws" {
  region = "us-east-1"
}

data "aws_ssm_parameter" "windows_ami" {
  name = "/aws/service/ami-windows-latest/Windows_Server-2022-English-Full-Base"
}

##########################
# Generate a Random Password
##########################
resource "random_password" "win_password" {
  length  = 16
  special = true
}

##########################
# Security Group
##########################
resource "aws_security_group" "winrm" {
  name        = "cef-builder-win-x64-sg"
  description = "Allow WinRM (and optionally RDP) inbound traffic"

  ingress {
    description = "WinRM (unencrypted) over HTTP"
    from_port   = 5985
    to_port     = 5986
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  ingress {
    description = "RDP"
    from_port   = 3389
    to_port     = 3389
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
resource "aws_instance" "windows" {
  ami                    = data.aws_ssm_parameter.windows_ami.value
  instance_type          = "c5.2xlarge"
  key_name               = "koen@pop-os"
  vpc_security_group_ids = [aws_security_group.winrm.id]

  tags = {
    Name = "cef-builder-win-x64"
  }

  root_block_device {
    volume_size           = 512
    volume_type           = "gp3"
    delete_on_termination = true
  }

  # User data configures WinRM and sets the Administrator password to the generated random value.
  user_data = <<EOF
<script>
  winrm quickconfig -q & winrm set winrm/config @{MaxTimeoutms="1800000"} & winrm set winrm/config/service @{AllowUnencrypted="true"} & winrm set winrm/config/service/auth @{Basic="true"}
</script>
<powershell>
  netsh advfirewall firewall add rule name="WinRM in" protocol=TCP dir=in profile=any localport=5985 remoteip=any localip=any action=allow
  # Set Administrator password to the generated random password
  $admin = [adsi]("WinNT://./Administrator, user")
  $admin.psbase.invoke("SetPassword", "${random_password.win_password.result}")
</powershell>
EOF

  # Connection uses WinRM over HTTP on port 5985 with the generated password.
  connection {
    type     = "winrm"
    host     = self.public_ip
    user     = "Administrator"
    password = random_password.win_password.result
    port     = 5985
    timeout  = "600m"
  }

  # Create a directory as an example remote command.
  provisioner "remote-exec" {
    inline = [
      "cmd /c mkdir c:\\code",
      "cmd /c mkdir c:\\code\\cefbuilder",
      "cmd /c mkdir c:\\code\\cefbuilder\\automate",
      "cmd /c mkdir c:\\code\\cefbuilder\\patches",
      "cmd /c mkdir c:\\code\\cefbuilder\\patches\\cef",
      "cmd /c mkdir c:\\code\\cefbuilder\\aws-windows-x64",
      "cmd /c mkdir c:\\code\\cefbuilder\\aws-windows-x64\\scripts"
    ]
  }

  # File provisioners to copy files to the instance

  # Copy configuration file (e.g., for WDK/Visual Studio)
  provisioner "file" {
    source      = "scripts/wdk.vsconfig"
    destination = "c:/code/wdk.vsconfig"
  }

  # Copy the setup scripts into the instance's Temp folder
  provisioner "file" {
    source      = "scripts/setup.bat"
    destination = "c:/Windows/Temp/setup.bat"
  }

  # Copy additional files (e.g., your repository and build scripts)
  provisioner "file" {
    source      = "../cef.branch"
    destination = "c:/code/cefbuilder/cef.branch"
  }
  provisioner "file" {
    source      = "../automate/automate-git.py"
    destination = "c:/code/cefbuilder/automate/automate-git.py"
  }
  provisioner "file" {
    source      = "../patches/apply_cef_patches.py"
    destination = "c:/code/cefbuilder/patches/apply_cef_patches.py"
  }
  provisioner "file" {
    source      = "../patches/cef/0001-file-paths.patch"
    destination = "c:/code/cefbuilder/patches/cef/0001-file-paths.patch"
  }
  provisioner "file" {
    source      = "scripts/build-x64.bat"
    destination = "c:/code/cefbuilder/aws-windows-x64/scripts/build-x64.bat"
  }
  provisioner "file" {
    source      = "scripts/build.bat"
    destination = "c:/code/cefbuilder/aws-windows-x64/scripts/build.bat"
  }
  provisioner "file" {
    source      = "scripts/build-arm64.bat"
    destination = "c:/code/cefbuilder/aws-windows-x64/scripts/build-arm64.bat"
  }

  # Execute the setup scripts in sequence
  provisioner "remote-exec" {
    inline = [
      "cmd /c c:/Windows/Temp/setup.bat"
    ]
  }
}

##########################
# Outputs
##########################
output "admin_password" {
  description = "The generated Windows Administrator password."
  value       = random_password.win_password.result
  sensitive   = true
}

output "private_ip" {
  description = "Private IP of the Windows instance."
  value       = aws_instance.windows.private_ip
}

output "public_ip" {
  description = "Public IP of the Windows instance."
  value       = aws_instance.windows.public_ip
}

output "instance_id" {
  description = "ID of the Windows instance."
  value       = aws_instance.windows.id
}

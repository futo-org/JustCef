#!/bin/sh
set -eu

cd /home/ubuntu/code
sudo -u ubuntu -H git clone https://github.com/chromiumembedded/cef.git chromium_git/cef
sudo -u ubuntu -H git clone https://github.com/chromium/chromium.git chromium_git/chromium/src
sudo -u ubuntu -H git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git depot_tools

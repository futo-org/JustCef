#!/bin/bash
export GN_DEFINES="is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome use_sysroot=true use_allocator=none symbol_level=1 is_cfi=false use_thin_lto=false"
export CEF_USE_GN=1
export CEF_ARCHIVE_FORMAT=tar.bz2
python3 ../automate/automate-git.py --download-dir=/home/koen/code/chromium_git --depot-tools-dir=/home/koen/code/depot_tools --no-update --no-debug-build --minimal-distrib-only --x64-build --build-target=cefsimple --force-build

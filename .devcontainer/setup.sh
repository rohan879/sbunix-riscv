#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

cd "$repo_root"

install_apt() {
  sudo apt-get update -qq
  sudo apt-get install -y -qq \
    build-essential \
    git curl wget \
    strace tmux vim htop screen \
    python3 python3-serial python3-pip \
    jq bc patch \
    libfdt-dev libfdt1 \
    libpixman-1-dev libpixman-1-0 \
    libslirp-dev libslirp0 \
    libglib2.0-dev \
    libncurses-dev \
    libssl-dev \
    autoconf automake autotools-dev \
    libmpc-dev libmpfr-dev libgmp-dev \
    gawk texinfo libtool patchutils zlib1g-dev \
    libexpat1-dev \
    ninja-build pkg-config \
    flex bison \
    libusb-1.0-0-dev \
    gh unzip ca-certificates
}

download_deps() {
  while ! command -v gh >/dev/null 2>&1; do
    sleep 1
  done

  tmp_zip=/tmp/os-dep.zip
  rm -f "$tmp_zip"
  gh release download ci-dependencies \
    --repo AdebayoBraimah/CSE506 \
    --pattern 'os-dep.zip' \
    --clobber \
    --output "$tmp_zip"
  cd /tmp
  unzip -q -o "$tmp_zip"
  rm -f "$tmp_zip"
}

update_submodules() {
  git submodule update --init --recursive --depth 1
}

install_apt &
apt_pid=$!
download_deps &
download_pid=$!
update_submodules &
submodule_pid=$!

wait "$apt_pid"
wait "$download_pid"
wait "$submodule_pid"

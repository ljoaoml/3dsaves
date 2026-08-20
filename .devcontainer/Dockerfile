# Konnect3DS build environment for GitHub Codespaces -- lets `make`/`make
# cia` run from any browser (Safari on an iPad works fine) or an SSH/terminal
# app, with no local devkitARM/MSYS2 install at all.
#
# Built on devkitPro's own official image, which already has devkitARM plus
# libctru/citro2d/citro3d (the `3ds-dev` package group) and a broad set of
# ported libraries (`3ds-portlibs`). `3ds-mbedtls` and `general-tools`
# (makerom/bannertool, needed for `make cia`) are installed explicitly with
# --needed, so this is a no-op if the base image already has them -- see the
# project Makefile's own header comment for exactly what it needs and why.
FROM devkitpro/devkitarm:latest

RUN dkp-pacman -Sy --noconfirm && \
    dkp-pacman -S --needed --noconfirm 3ds-mbedtls general-tools && \
    yes | dkp-pacman -Scc

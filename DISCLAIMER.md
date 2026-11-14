# Disclaimer

This repository documents an **experimental** CPU and GPU overclock for a
specific phone: the Samsung Galaxy A03 Core (SM-A032F) with the Unisoc SC9863A
SoC and PowerVR Rogue GE8322 GPU. It is not a supported product, and it is not
affiliated with Samsung, Unisoc, Imagination Technologies, Google, or any
carrier.

## Read this before flashing anything

Overclocking changes the operating conditions of the SoC outside the vendor's
validated limits. Even when a stress test passes, the device can fail later.
Possible consequences include, but are not limited to:

- boot loops, a black screen, or a device that will not start;
- GPU hangs, rendering corruption, application crashes, or kernel panics;
- data loss or filesystem corruption if the device hangs during a write;
- increased heat, reduced battery life, and accelerated silicon aging;
- loss of warranty, Knox state changes, or a device that a service centre will
  not repair.

You are responsible for your own device, data, and actions. If you are not
comfortable recovering a phone from a bad boot image with download mode/Odin,
do not start.

## Scope and assumptions

- The guide assumes an **unlocked bootloader**, **root**, and a working
  **download-mode/Odin** recovery path. Without a recovery path, a bad boot
  image can leave the phone unusable.
- The guide changes only the `boot` partition. It does not modify the
  bootloader, `pm_sys`, `sml`, `trustos`, the `dtbo` partition, or user data.
- The guide assumes the vendor kernel and firmware listed in the README. A
  different firmware build may have different offsets, device-tree layouts, or
  OPP tables. Verify every offset against your own image before writing.
- The GPU has **no adjustable voltage rail** in this design. The 650 MHz and
  700 MHz steps run at the fixed `vddcore` (900 mV). There is no voltage margin
  to recover if a sample is unstable.
- The DDR overclock to 1000 MHz is **not** part of this guide and is not
  recommended. 933 MHz is the top OPP in the platform's PM firmware.

## No warranty

The scripts, patches, and documentation are provided "as is", without warranty
of any kind, express or implied, including but not limited to the warranties of
merchantability, fitness for a particular purpose, and non-infringement. In no
event shall the authors be liable for any claim, damages, or other liability
arising from the use of this material.

## Legal

Use this only on a device you own or are explicitly authorized to modify.
Unlocking a bootloader and modifying system software may violate the terms of
your warranty, carrier agreement, or local law. You are responsible for
complying with the rules that apply to you.

## Privacy

The repository intentionally contains no boot images, kernel images, GPU
firmware, vendor blobs, device serial numbers, IMEI/MAC addresses, phone IP
addresses, SSH credentials, Wi-Fi names, or personal contact details. Build
your own images from your own device, and do not commit your own identifiers
to a public fork.

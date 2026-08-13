This repository contains the configuration and build logic for the native integration of the [vDS (Virtual DualSense) Driver and Daemon](https://github.com) (Target Version: v0.4.0), originally developed by [hurryman2212](https://github.com), into a custom Bazzite system image. The implementation is executed entirely during the OCI container build process, eliminating the need for Distrobox or manual modifications on the running host system.

## System Architecture and Core Components

### L2CAP Socket Hardening and SELinux Bypass
Communication relies on utilizing `SOCK_SEQPACKET` (`AF_BLUETOOTH`, `BTPROTO_L2CAP`) on PSM channels `0x0011` and `0x0013`. This maintains standard security compliance and prevents SELinux denials within the `init_t` service context on immutable, read-only operating systems like Bazzite or Fedora Silverblue without lowering system security boundaries. To pass the channels flawlessly into the userspace daemon, the `--compat` flag is strictly required inside the BlueZ systemd unit. The original BlueZ input plugin remains active for the initial handshake phase.

### Monolithic Daemon Operation
The `vdsd.service` runs argumentless as a monolithic system service. Controller mapping is managed in a purely state-based manner via a local SQLite database (`vdsd.db`).

### Kernel ID Limitations and WirePlumber Audio Spoofing
The strict 15-character limitation for the ALSA card ID (`card->id`) inside the Linux kernel prevents direct naming as `WirelessControl` within the kernel module code, as the standard `snd-usb-audio` driver strips and compresses the string down to the word `Controller` upon registration. Consequently, the renaming and normalization are handled exclusively within the userspace application layer via WirePlumber (version 0.5+). A highly specific rule intercepts the device and masks it to the exact `WirelessController` name expected by Steam and Proton.

### Isolated Bluetooth Header Generation
Compiling against external system BlueZ headers often fails during container builds due to missing development packages or mismatched versions. To bypass this dependency lock entirely, the build pipeline generates standalone, localized Bluetooth and L2CAP C-headers on-the-fly. This guarantees a consistent, self-contained compilation context without relying on the host or container base image providing matching Bluetooth development libraries.

## Automated Workflow (Zero Terminal Inputs)

1. **Initial Connection**: The controller connects normally via BlueZ and is mounted in the kernel under `/devices/virtual/misc/uhid/`.
2. **Udev Interception**: The udev rule triggers within the stable `input` subsystem, matching exclusively on the main device, and extracts the MAC address using the kernel attribute `$attr{properties/uniq}`.
3. **Database Registration**: Udev executes `/usr/bin/vdsctl attach` to write the controller's MAC address directly into the daemon's database.
4. **Daemon Handshake**: The daemon's `SOCK_SEQPACKET` path attaches to the active L2CAP channels and initializes the virtual USB interface (`/dev/vds0`).

## Packaging Logic inside the OCI Container (BlueBuild)

To prevent an unresolvable transaction lock on `/usr/share/rpm/.rpm.lock` caused by the host system's ostree export process accessing the RPM database concurrently, running `rpm -ivh` directly inside the container build is prohibited. Instead, the RPM is compiled via `rpmbuild` strictly as a standalone file. Installation within the container is performed in a lock-free, atomically clean manner by extracting the binary payload directly into the root directory using `rpm2cpio`.

## Build and Installation Pipeline
The automated build process in the `Containerfile` flows through four conceptual phases:

* **Download**: Clones the upstream repository and targets the running kernel version.
* **Modification**: Injects localized Bluetooth headers and deploys customized WirePlumber and udev rules.
* **Packaging**: Aggregates all staged files and compiles them into an isolated RPM package via `rpmbuild`.
* **Deployment**: Extracts the raw binary payloads directly to root using `rpm2cpio` and enables all targets cleanly.

## Validation and Operational Testing

To verify the integrity of the entire automation pipeline after system boot, execute the following diagnostic commands inside a terminal:

1. **Verify the Kernel Module**:
   ```bash
   lsmod | grep vds_hcd
   ```
   The module `vds_hcd` must be listed as active.

2. **Check the Active ALSA ID**:
   ```bash
   cat /sys/class/sound/card*/id | grep Controller
   ```
   Should output the unmasked kernel hardware ID string `Controller`.

3. **Verify Userspace Recognition**:
   ```bash
   wpctl status | grep Controller
   ```
   The sound card must display exclusively under the alias `WirelessController` without duplicate Bluetooth audio listings.

4. **Exclusive HD Rumble Quadraphonic Test**:
   Terminate Steam entirely before running this test (`pkill -9 steam`) to avoid device allocation conflicts (error `-16` / Device or resource busy). The test route must bypass standard mixers and target the exact PipeWire node:
   ```bash
   speaker-test -D pipewire:NODE=alsa_output.usb-Sony_Interactive_Entertainment_WirelessController-00.pro-audio -c 4 -t sine -f 80
   ```
   The sine wave sweep must progress smoothly across all 4 channels without interruption (error `-4`) and trigger distinct physical vibrations on the controller hardware.

## In-Game Application (Example: Horizon Forbidden West)

For games to directly access the custom haptic triggers and integrated controller speakers, specific configurations must be applied inside Steam:

1. **Disable Steam Input**: Right-click the game in your library, navigate to **Properties...** -> **Controller**, and select **Disable Steam Input** from the dropdown menu. This prevents the Steam input translation wrapper from intercepting raw hardware calls.
2. **Enforce the Compatibility Layer**: Under the **Compatibility** tab, check the box to force a specific Steam Play tool and select **GE-Proton 11-5** (or newer).
3. **Apply Startup Parameters**: Navigate to the **General** tab and insert the following variable inside the Launch Options entry field:
   ```bash
   PROTON_USE_PIPEWIRE=0 %command%
   ```
   This disables Proton's internal PipeWire audio routing layer for this specific instance, directing haptic and audio telemetry straight to the configured ALSA vDS node.

## Architecture Notes & Constraints

* **Requirement: Bind Udev Detection to the Input Subsystem**  
  * *Context*: Udev must track devices within the stable `input` layer to properly evaluate `$attr{properties/uniq}` for unique device identification.
* **Requirement: Respect Read-Only Atomic Filesystem Boundaries**  
  * *Context*: Bazzite and Fedora Silverblue utilize strict system security policies. System files and configurations must be deployed cleanly during image creation via standard package extraction without runtime overrides.
* **Limitation: Single-Device Constraint**  
  * *Context*: The software routing relies on a fixed ALSA identifier mapping (`Controller`). Connecting multiple DualSense controllers simultaneously will cause hardware naming collisions within the userspace sound layer.

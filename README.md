# AirBeam (Fork)

> A solution to address audio latency when using a HomePod as the system output device on macOS over RAOP.

<img src="./resources/AirBeam.png" width="200" height="200">

[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buymeacoffee.com/boidu)

## Why This Fork?

The [original AirBeam](https://github.com/ChenKS12138/AirBeam) is a brilliant project that bypasses macOS's native AirPlay stack to stream audio to HomePods with significantly lower latency. However, while trying to get it working, I ran into several crashes and rough edges that made the setup process painful - segfaults during Bonjour discovery, silent connection failures, and no way to keep the output device selected without babysitting it.

This fork fixes those bugs and adds convenience tooling (auto-switch script, menubar toggle, one-command install) to make the whole experience smoother.

### What's Fixed

- Crash during Bonjour service discovery (use-after-free bug)
- Silent connection failures - errors are now properly detected and reported
- AirBeamDoctor crash when the HomePod can't be reached
- Missing error details when TCP connections fail

### Audio Quality Improvements

- **Bulk FIFO buffer** - replaced byte-by-byte audio buffer (1,408 lock/unlock cycles per chunk) with bulk `memcpy` operations (~1 lock per chunk), reducing mutex contention by ~1000x
- **Precision timing** - replaced imprecise `sleep_for` (~1ms granularity) with `mach_wait_until` for sub-microsecond packet scheduling, and fixed a bug where the original sleep calculation was ~4.3x too long
- **Gradual drift correction** - instead of hard clock resets that cause audible clicks, timing drift is corrected smoothly at 5% per chunk
- **Lost packet retransmission** - HomePod requests for missed packets were silently ignored; now retransmitted from a 512-packet ring buffer
- **Atomic timestamp access** - fixed a data race between the audio consumer thread and sync thread that caused clicks during idle/silence
- **Pre-allocated send buffers** - eliminated ~250 heap allocations/sec from the audio hot path
- **Real-time thread priority** - audio consumer thread now runs with macOS `THREAD_TIME_CONSTRAINT_POLICY` to prevent scheduling delays
- **UDP socket optimization** - increased send buffer (256KB) and DSCP Expedited Forwarding marking so routers prioritize audio packets over bulk traffic
- **Zero-copy send path** - audio packets are sent directly from the serialization buffer with a pre-resolved socket address, eliminating per-packet `inet_pton` calls and string copies
- **Non-blocking HAL writes** - the CoreAudio real-time callback no longer blocks indefinitely if the FIFO is full, preventing system-wide audio freezes
- **TCP_NODELAY on RTSP** - disabled Nagle's algorithm so keepalive and volume control messages are sent immediately instead of being buffered

### Stability Improvements

- **Removed all 18 `exit(-1)` calls** - the original code killed `coreaudiod` on any transient network error (Wi-Fi blip, slow HomePod response). Now startup failures return errors gracefully, background threads retry on transient errors, and the audio hot path logs and continues

## Prerequisites

Before you start, make sure you have the following installed:

1. **Homebrew** - If you don't have it, open Terminal and run:
   ```shell
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. **Ninja** (build tool):
   ```shell
   brew install ninja
   ```

3. **SwitchAudioSource** (needed for the auto-switch script):
   ```shell
   brew install switchaudio-osx
   ```

4. **CMake** (build system):
   ```shell
   brew install cmake
   ```

## Step 1: Clone and Install

Open Terminal (or your preferred terminal app like Ghostty, iTerm, etc.) and run:

```shell
git clone https://github.com/boidushya/AirBeam.git
cd AirBeam
bash ./reinstall.sh
```

This will:
- Build AirBeam from source
- Install the audio plugin to your system
- Sign it so macOS allows it to load
- Restart the audio system

You'll be asked for your password (for `sudo`) - this is normal, the plugin needs to be installed to a system directory.

After a few seconds, you should see a new output device in **System Settings → Sound → Output** with a name like `D6D63AF4F8EF@Bedroom` (your HomePod's name). Select it, and audio will stream to your HomePod with low latency!

## Step 2: Grant Local Network Permission

> **This is critical!** Without this, AirBeam cannot reach your HomePod.

On macOS Sequoia and later, apps need explicit permission to talk to other devices on your network.

1. Open **System Settings**
2. Go to **Privacy & Security → Local Network**
3. Find your terminal app (Terminal, Ghostty, iTerm, etc.) and **turn it on**

If you skip this step, you'll get "No route to host" errors.

## Step 3: Set Up Auto-Switch (Optional but Recommended)

macOS has a habit of switching your audio output away from the AirBeam device (e.g., after waking from sleep or when the HomePod briefly disconnects). The auto-switch script fixes this by automatically switching back.

### 3a: Configure the script

Open `airbeam-watch.sh` in a text editor and update two things:

1. **`DEVICE`** - Set this to your AirBeam device name. To find it, run:
   ```shell
   SwitchAudioSource -a
   ```
   Look for the entry that looks like `XXXXXXXXXXXX@YourHomePodName`.

2. **`INTENTIONAL`** - Add any devices you'd **deliberately** switch to. The script won't override these. For example, if you sometimes use AirPods, add them here. Device names must match exactly what `SwitchAudioSource -a` shows.

### 3b: Install the LaunchAgent

This makes the auto-switch script run automatically in the background, even after restarting your Mac:

```shell
bash ./install_launchagent.sh
```

This generates the LaunchAgent with the correct path for your system and starts it. To verify it's running:
```shell
launchctl list | grep airbeam
```

You should see a line with `com.airbeam.watch`.

## Step 4: Install the Menubar Toggle (Optional)

This adds a small speaker icon to your menubar that lets you pause/resume auto-switching with one click. Useful for when you're away from home or want to use a different output without the script fighting you.

### Build and install:

```shell
cd AirBeam
swiftc -o AirBeamToggle AirBeamToggle.swift -framework Cocoa
mkdir -p AirBeamToggle.app/Contents/MacOS
cp AirBeamToggle AirBeamToggle.app/Contents/MacOS/
cp AirBeamToggle.app /Applications/
open /Applications/AirBeamToggle.app
```

### Make it launch on login:

1. Open **System Settings**
2. Go to **General → Login Items**
3. Click the **+** button
4. Navigate to **Applications** and select **AirBeamToggle**

### How it works:

- **Speaker icon** in the menubar = auto-switch is active
- **Slashed speaker icon** = auto-switch is paused
- Click the icon and select **Pause Auto-Switch** or **Resume Auto-Switch**

You can also toggle from the terminal: `touch ~/.airbeam-pause` to pause, `rm ~/.airbeam-pause` to resume.

## Troubleshooting

### "The output device disappeared"

This can happen after a macOS update or system restart. Run:

```shell
cd AirBeam
bash ./reinstall.sh
```

### "No route to host" error

1. Make sure your Mac and HomePod are on the **same Wi-Fi network**
2. Check that your terminal app has **Local Network** permission (see Step 2)
3. Try restarting the HomePod - unplug it, wait 10 seconds, plug it back in

### "I can't hear anything"

1. Make sure the AirBeam device is selected as your output in **System Settings → Sound**
2. Check that the volume is turned up both on your Mac and on the HomePod
3. Try running the doctor tool to verify the connection:
   ```shell
   cd AirBeam
   bash ./run_airbeam_doctor.sh
   ```
   This will scan for HomePods on your network, let you pick one, and play a test sound. If you hear audio, the connection works and the issue is with the HAL plugin - try `bash ./reinstall.sh`.

### "The auto-switch keeps selecting the wrong device"

Edit `airbeam-watch.sh` and make sure the `DEVICE` variable matches your AirBeam device name exactly. Run `SwitchAudioSource -a` to see the correct names.

---

## Original Project

*Everything below is from the original [ChenKS12138/AirBeam](https://github.com/ChenKS12138/AirBeam). This fork wouldn't exist without their work - please consider supporting the original author!*

[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/chenks12138)

### 🧪 Compare with Native Speaker & AirPlay

🖥️ Test Environment

* **Laptop**: MacBook Pro 14-inch (2021)
  * **OS**: macOS Sequoia 15.4
  * **Network**: Wired Ethernet connection
* **Speaker**: HomePod mini (2020)
  * **OS**: 18.5
  * **Network**: Connected via 5GHz Wi-Fi

> **⚠️ PLEASE TURN OFF MUTE BEFORE PLAYING THE VIDEOS!**

| Native Speaker  | Airplay | AirBeam |
| ------------- | ------------- | ------------- |
| <video src="https://github.com/user-attachments/assets/2b89ad33-e055-4cbe-95da-fc29a9156109.mp4">  | <video src="https://github.com/user-attachments/assets/40815cc9-0c97-4dd4-8b64-6e78be526605.mp4">| <video src="https://github.com/user-attachments/assets/defb753d-1c00-47cb-9ab6-cec83c31e919.mp4">|


###  📦 Installation

There are several ways:

* ~~⬇️ Download [the latest release on Github](https://github.com/ChenKS12138/AirBeam/releases)~~ There're some issue with pre-built release, I will fix it.
* 🚀 Clone it and build it yourself

### ⚙️ Usage

1. Launch the app
2. Click **Install**
3. Select your output device in the macOS audio control panel

<img width="748" alt="image" src="https://github.com/user-attachments/assets/24f7904b-7238-4815-89bb-3fc4519b4269" />


### 🛠️ Build from Source

```shell
git clone git@github.com:ChenKS12138/AirBeam.git
cd AirBeam
cmake -S . -B build
# or try `cmake -S . -B build -GNinja`, I prefer to use Ninja, it's faster.
cmake --build build --target AirBeam
open build/source/AirBeam/AirBeam.app
```

### 🪛 Troubleshooting

```shell
git clone git@github.com:ChenKS12138/AirBeam.git
cd AirBeam
bash ./run_airbeam_doctor.sh

# This script will search for RAOP services on the network, you need to enter the number to select one, the script will transmit audio to the RAOP service, and print debug logs to the `mylog.log` file.
# For feedback, please provide the `mylog.log` file.
```

<img width="932" alt="image" src="https://github.com/user-attachments/assets/73ae18b9-c6f1-4437-95fb-0584353d2120" />


### 🙏 Credits

Parts of this project were inspired by or adapted from the following open source projects:

- [gavv/libASPL](https://github.com/gavv/libASPL) – for creating macOS Audio Server plugins.
- [LinusU/rust-roap-player](https://github.com/LinusU/rust-raop-player) – reference for AirPlay integration.

Huge thanks to their authors and contributors! 💖

### 🤝 Contributing

Contributions are welcome! Feel free to open an issue or submit a PR.

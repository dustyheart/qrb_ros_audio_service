<div align="center">
  <h1>QRB ROS Audio Service</h1>
  <p align="center">
  </p>
  <p>This ROS package delivers essential audio capabilities for playback and recording</p>

  <a href="https://ubuntu.com/download/qualcomm-iot" target="_blank"><img src="https://img.shields.io/badge/Qualcomm%20Ubuntu-E95420?style=for-the-badge&logo=ubuntu&logoColor=white" alt="Qualcomm Ubuntu"></a>
  <a href="https://docs.ros.org/en/jazzy/" target="_blank"><img src="https://img.shields.io/badge/ROS%20Jazzy-1c428a?style=for-the-badge&logo=ros&logoColor=white" alt="Jazzy"></a>
 
</div>

> [!NOTE]
> This repository is currently migrating to ROS 2 Lyrical. Documentation may still reference Jazzy, and some features may not be fully supported yet.

---

## 👋 Overview

> 📌 **qrb_ros_audio_service is a ROS package that provides core audio functionalities. It serves as the primary interface for audio playback and recording within the ROS ecosystem.**

<div align="left">
  <img src="./docs/assets/architecture.png" alt="architecture" width="600">
</div>

- QRB ROS Audio Service : A ROS node that creates a service server, responds to requests from the application, and translates them into operations on the Audio Manager.
- QRB Audio Manager : Executes audio operations provided by the Audio Service, based on stream configurations.
- QRB Audio Common Lib : Provides audio functionalities by calling PulseAudio or ALSA APIs, depending on which audio backend is available on the system. Streaming capture runs through an optional processing chain (WebRTC echo cancellation, noise suppression and voice activity detection) on a per-stream worker thread.

## 🔎 Table of Contents

> 📌 If the content is extensive, we recommend adding a table of contents.

  * [APIs](#-apis)
  * [Supported Targets](#-supported-targets)
  * [Installation](#-installation)
  * [Usage](#-usage)
  * [Voice front-end example](#-voice-front-end-example)
  * [Build from Source](#-build-from-source)
  * [Contributing](#-contributing)
  * [License](#-license)

## ⚓ APIs

> 📌 `qrb_ros_audio_service` APIs

**ROS Interfaces**
<table>
  <tr>
    <th>Interface</th>
    <th>Name</th>
    <th>Type</th>
    <td>Description</td>
  </tr>
  <tr>
    <td>Node</td>
    <td>/audio_service</td>
    <td>rclcpp component qrb_ros::audio_service::AudioServer</td>
    <td>Node name (parameters are set on this name). There is no standalone executable: use a launch file or <code>ros2 component standalone</code>.</td>
  </tr>
  <tr>
    <td>Service</td>
    <td>/audio_server</td>
    <td>qrb_ros_audio_service_msgs::srv::AudioRequest</td>
    <td>Allows ROS applications to send service requests for audio playback and recording. Note: the service name differs from the node name.</td>
  </tr>
  <tr>
    <td>Subscriber</td>
    <td>&lt;topic_name&gt;</td>
    <td>qrb_ros_audio_service_msgs::msg::AudioData</td>
    <td>Used for streaming audio playback. Subscribes to raw audio data on the topic given by <code>topic_name</code> in the create request.</td>
  </tr>
  <tr>
    <td>Publisher</td>
    <td>&lt;topic_name&gt;</td>
    <td>qrb_ros_audio_service_msgs::msg::AudioData</td>
    <td>Used for streaming audio recording (<code>pub_pcm=true</code>). Publishes captured (and, if configured, processed) PCM on the topic given by <code>topic_name</code>; always set <code>topic_name</code> explicitly.</td>
  </tr>
  <tr>
    <td>Publisher</td>
    <td>&lt;topic_name&gt;&lt;vad_topic_suffix&gt;</td>
    <td>std_msgs::msg::Bool</td>
    <td>Voice activity decision, one message per PCM message, only while a <code>vad</code> stage is in <code>capture_processors</code>. Default topic e.g. <code>/qrb_audiodata_vad</code>. Interim type, to be replaced by a dedicated VAD message.</td>
  </tr>
</table>

#### ROS parameters
  <table>
    <tr>
      <th>Name</th>
      <th>Type</th>
      <th>Description</th>
      <td>Default Value</td>
    </tr>
    <tr>
      <td>enable_latency_log</td>
      <td>bool</td>
      <td>enable latency log print</td>
      <td>false</td>
    </tr>
    <tr>
      <td>capture_device</td>
      <td>string</td>
      <td>PipeWire/PulseAudio source for recording. Empty = default source. An unknown name makes <code>create</code> fail and the log lists the available sources.</td>
      <td>""</td>
    </tr>
    <tr>
      <td>playback_device</td>
      <td>string</td>
      <td>PipeWire/PulseAudio sink for playback. Empty = default sink.</td>
      <td>""</td>
    </tr>
    <tr>
      <td>capture_fragment_duration_ms</td>
      <td>int (0-2000)</td>
      <td>Size of each recorded chunk / AudioData message in ms. 0 = library default (80 ms). 20 is a good value for voice.</td>
      <td>0</td>
    </tr>
    <tr>
      <td>playback_buffer_duration_ms</td>
      <td>int (0-2000)</td>
      <td>Playback pre-buffer size in ms. 0 = library default (300 ms).</td>
      <td>0</td>
    </tr>
    <tr>
      <td>capture_processors</td>
      <td>string[]</td>
      <td>Processing chain for streaming capture, in order <code>aec</code>, <code>ns</code>, <code>vad</code> (any subset). Each entry is <code>name</code> or <code>name:key=value,...</code>, e.g. <code>ns:level=high</code>. Empty = raw capture. <code>aec</code> needs webrtc-audio-processing 1.x, <code>ns</code>/<code>vad</code> work with 1.x or the system 0.3.1 (see <a href="#-build-from-source">Build from Source</a>).</td>
      <td>[]</td>
    </tr>
    <tr>
      <td>ns_level</td>
      <td>string</td>
      <td>Noise suppression level: <code>low</code>, <code>moderate</code>, <code>high</code>, <code>very_high</code>.</td>
      <td>moderate</td>
    </tr>
    <tr>
      <td>vad_likelihood</td>
      <td>string</td>
      <td>VAD aggressiveness: <code>very_low</code>, <code>low</code>, <code>moderate</code>, <code>high</code>. Higher reports voice more readily (fewer missed words, more false positives).</td>
      <td>very_low</td>
    </tr>
    <tr>
      <td>vad_hangover_ms</td>
      <td>int (0-2000)</td>
      <td>Keep the VAD decision "voice" for this long after the last voiced 10 ms block.</td>
      <td>300</td>
    </tr>
    <tr>
      <td>vad_topic_suffix</td>
      <td>string</td>
      <td>Suffix of the VAD topic, appended to the PCM <code>topic_name</code>.</td>
      <td>_vad</td>
    </tr>
    <tr>
      <td>aec_reference_device</td>
      <td>string</td>
      <td>Echo reference for <code>aec</code>: the monitor source of the sink that drives the speaker. If it cannot be opened or stalls, the stream keeps running and AEC passes audio through (logged).</td>
      <td>@DEFAULT_MONITOR@</td>
    </tr>
  </table>

> [!NOTE]
> All capture parameters apply to streams **created after** the change (`ros2 param set` updates the defaults; running streams are not rebuilt). A `ros2 param set` that would make the processing chain invalid is rejected as a whole.
> The WebRTC processors run on 16-bit PCM at 8/16/32/48 kHz with 1 or 2 channels; a stream in another format keeps running with that stage bypassed (logged).

> 📌  `qrb_audio_common_lib` APIs

<table>
  <tr>
    <th>Function</th>
    <th>Parameters</th>
    <th>Description</th>
  </tr>
  <tr>
    <td>AudioBackend detect_audio_backend()</td>
    <td>None</td>
    <td>Detect which audio backend is available on the system (PulseAudio is preferred, ALSA is used as a fallback). Returns the detected AudioBackend (INVALID/ALSA/PULSEAUDIO).</td>
  </tr>
  <tr>
    <td>uint32_t audio_stream_open(const AudioStreamInfo & stream_info, stream_event_callback_func event_callback)</td>
    <td>
      <strong>stream_info</strong>: A structure that specifies the configuration of the stream (format, rate, channels, type, file_path, volume, device, need_timestamp, pcm_mode, repeat).<br>
      <strong>event_callback</strong>: Callback function invoked for stream events (start/stop/abort/timestamp/data/eos).
    </td>
    <td>Open a stream, returns stream_handle on success, 0 on failure.</td>
  </tr>
  <tr>
    <td>int audio_stream_start(uint32_t stream_handle)</td>
    <td><strong>stream_handle</strong>: A unique identifier for the stream.</td>
    <td>Start stream, return 0 on succeed.</td>
  </tr>
  <tr>
    <td>int audio_stream_mute(uint32_t stream_handle, bool mute)</td>
    <td>
      <strong>stream_handle</strong>: A unique identifier for stream.<br>
      <strong>mute</strong>: Boolean flag indicating whether to mute the stream.
    </td>
    <td>Mute or unmute stream. return 0 on succeed.</td>
  </tr>
  <tr>
    <td>int audio_stream_stop(uint32_t stream_handle)</td>
    <td><strong>stream_handle</strong>: A unique identifier for stream.</td>
    <td>Stop stream, return 0 on succeed</td>
  </tr>
  <tr>
    <td>int audio_stream_close(uint32_t stream_handle)</td>
    <td><strong>stream_handle</strong>: A unique identifier for stream.</td>
    <td>Close stream and release its resources, return 0 on succeed.</td>
  </tr>
  <tr>
    <td>int audio_stream_write(uint32_t stream_handle, const void * buf, size_t length)</td>
    <td>
      <strong>stream_handle</strong>: A unique identifier for stream.<br>
      <strong>buf</strong>: Pointer to the data buffer.<br>
      <strong>length</strong>: Size to write.
    </td>
    <td>Write buffer to a PCM-mode playback stream, return actually written length, negative value on failure.</td>
  </tr>
</table>

## 🎯 Supported Targets

<table >
  <tr>
    <th>Development Hardware</th>
    <td>Qualcomm Dragonwing™ IQ-9075 EVK</td>
  </tr>
  <tr>
    <th>Hardware Overview</th>
    <th><a href="https://www.qualcomm.com/products/internet-of-things/industrial-processors/iq9-series/iq-9075"><img src="https://s7d1.scene7.com/is/image/dmqualcommprod/dragonwing-IQ-9075-EVK?$QC_Responsive$&fmt=png-alpha" width="160"></a></th>
  </tr>
</table>

---

## ✨ Installation

> [!IMPORTANT]
> **PREREQUISITES**: The following steps need to be run on **Qualcomm Ubuntu** and **ROS Jazzy**.<br>
> Reference [Install Ubuntu on Qualcomm IoT Platforms](https://ubuntu.com/download/qualcomm-iot) and [Install ROS Jazzy](https://docs.ros.org/en/jazzy/index.html) to setup environment. <br>
> For Qualcomm Linux, please check out the [Qualcomm Intelligent Robotics Product SDK](https://docs.qualcomm.com/bundle/publicresource/topics/80-70018-265/introduction_1.html?vproduct=1601111740013072&version=1.4&facet=Qualcomm%20Intelligent%20Robotics%20Product%20(QIRP)%20SDK) documents.

### Add Qualcomm IOT PPA for Ubuntu:

```bash
# Install Qualcomm PPA
sudo add-apt-repository ppa:ubuntu-qcom-iot/qcom-ppa
sudo add-apt-repository ppa:ubuntu-qcom-iot/qirp
sudo apt update
```

### Install Packages

```bash
# Install QRB ROS Audio packages
sudo apt ros-jazzy-qrb-ros-audio-service
```

## 🚀 Usage

1. Use this launch file to run this package (no processing, default devices).
    ```bash
    source /opt/ros/jazzy/setup.sh
    ros2 launch qrb_ros_audio_service audio_service.launch.py
    ```
    For voice capture with echo cancellation / noise suppression / VAD see [Voice front-end example](#-voice-front-end-example).
2. Run test cases on a third ssh terminal.
 - push music file to device path like /tmp/xxx.wav.
 - download test script.
    ```bash
    wget https://raw.githubusercontent.com/qualcomm-qrb-ros/qrb_ros_audio_service/main/tests/audio_service_test.py
    ```
 - setup ros2 env.
     ```bash
    source /opt/ros/jazzy/setup.sh
    ```
 - Step-by-step playback (sound will output on speaker)
    ```bash
    python3 audio_service_test.py --type='playback' --source='/tmp/xxx.wav' --volume=100
    ```
 - Step-by-step record (will record sound input from mic and save to file)
    ```bash
    python3 audio_service_test.py --type='record' --source='/tmp/rec.wav' --channels=1 --sample_rate=16000 --sample_format=16
    ```
 - Streaming playback
    ```bash
    python3 audio_service_test.py --type='playback' --channels=1 --sample_rate=16000 --sample_format=16 --pub_pcm=True --volume=100 --topic_name='loopback'
    ```
 - Streaming record (if start Streaming playback and Streaming record sound will loopback from mic to speaker)
    ```bash
    python3 audio_service_test.py --type='record' --source='/tmp/rec.wav' --channels=1 --sample_rate=16000 --sample_format=16 --topic_name='loopback'
    ```
 - Raw service calls (the service is `/audio_server`; `create` only creates the stream, `start` begins capture)
    ```bash
    ros2 service call /audio_server qrb_ros_audio_service_msgs/srv/AudioRequest \
      "{command: create, type: record, pub_pcm: true, topic_name: qrb_audiodata, audio_info: {channels: 1, sample_rate: 16000, sample_format: 16}}"
    ros2 service call /audio_server qrb_ros_audio_service_msgs/srv/AudioRequest "{command: start, stream_handle: <handle from create>}"
    # ... then "stop" and "release" with the same stream_handle
    ```
    On failure the response only has `success: false`; the reason is logged by the node as `[error_code] message` (e.g. `[device_unavailable] capture device 'x' not found; available: ...`).

## 🎙 Voice front-end example

`voice_frontend.launch.py` starts the service with the capture chain `aec, ns, vad`, and optionally the example client `voice_frontend_client.py`. The client is the minimal ASR/dialog-side integration: create + start a streaming record stream, consume `AudioData` and the VAD topic, stop + release on exit.

```bash
# service + example client, 16 kHz mono (typical ASR input), 30 s
ros2 launch qrb_ros_audio_service voice_frontend.launch.py start_client:=true sample_rate:=16000 client_duration:=30

# 48 kHz, explicit mic and speaker monitor, stronger NS, more sensitive VAD
ros2 launch qrb_ros_audio_service voice_frontend.launch.py start_client:=true sample_rate:=48000 \
    capture_device:=alsa_input.platform-sound.HiFi__hw_LEMANSEVK_1__source \
    aec_reference_device:=alsa_output.platform-sound.HiFi__hw_LEMANSEVK_0__sink.monitor \
    ns_level:=high vad_likelihood:=low

# service only, NS + VAD without echo cancellation; run your own client against it
ros2 launch qrb_ros_audio_service voice_frontend.launch.py capture_processors:=ns,vad
ros2 run qrb_ros_audio_service voice_frontend_client.py --sample-rate 16000 --save-wav /tmp/voice.wav
```

Launch arguments: `capture_device`, `playback_device`, `capture_fragment_duration_ms` (default 20), `capture_processors` (comma-separated, default `aec,ns,vad`, `none` = raw), `ns_level`, `vad_likelihood`, `vad_hangover_ms`, `vad_topic_suffix`, `aec_reference_device`; client: `start_client`, `sample_rate`, `channels`, `topic_name` (default `voice_pcm`), `client_duration`. Run `ros2 launch qrb_ros_audio_service voice_frontend.launch.py --show-args` for the full list.

Client output (one line per second, plus each VAD transition):

```text
[voice_frontend_client]: stream 1767050224 started: 16000 Hz, 1 ch, PCM on "voice_pcm", VAD on "voice_pcm_vad"
[voice_frontend_client]:    3.0 s: 151 chunks,    3.0 s audio, level  -53.6 dBFS, vad msgs 151, speech=no
[voice_frontend_client]: [   7.49 s] speech START
[voice_frontend_client]: [   7.86 s] speech END (0.37 s)
```

In your own node: subscribe to `<topic_name>` (`AudioData.data` = 16-bit little-endian interleaved PCM in the requested format) and `<topic_name>_vad` (`std_msgs/Bool`, published right after each PCM message), with a reliable QoS.

Notes:
- **Echo reference.** AEC subtracts what the device itself plays. `aec_reference_device` must be the monitor of the sink that feeds the speaker (`wpctl status` / `pactl list short sources` show `<sink>.monitor`). Without a speaker, use `capture_processors:=ns,vad`.
- **Sample rate** is chosen per stream by the client (`AudioRequest.audio_info`). On the IQ-9075 EVK the AEC3+NS block costs about 0.25 ms per 10 ms block at 16 kHz and about 0.9 ms at 48 kHz, VAD 6-24 us.
- **Latency.** Processing adds a fixed 14-15 ms (AEC3+NS at 16 or 48 kHz); per-chunk capture timestamps are corrected for it.
---

## 👨‍💻 Build from Source

### Step 1: Install dependencies: 
```bash
# Install build tools and dependencies
sudo add-apt-repository ppa:ubuntu-qcom-iot/qcom-ppa
sudo add-apt-repository ppa:ubuntu-qcom-iot/qirp
sudo apt update
sudo apt install build-essential cmake pkg-config

# Install ROS2 Jazzy (if not already installed)
# Follow instructions at https://docs.ros.org/en/jazzy/Installation.html
sudo apt install ros-jazzy-rclcpp ros-jazzy-rclcpp-components ros-jazzy-ament-cmake-auto ros-jazzy-std-msgs libpulse-dev libsndfile1-dev libasound2-dev

# For the WebRTC processors (webrtc_audio_processing_vendor builds webrtc-audio-processing 1.3)
sudo apt install meson ninja-build libabsl-dev
# Optional fallback: system webrtc-audio-processing 0.3.1 (ns/vad only, no aec)
sudo apt install libwebrtc-audio-processing-dev
```

Or let rosdep resolve the build dependencies: `rosdep install --from-paths src --ignore-src -y`.

> [!NOTE]
> `qrb_ros_audio_service_msgs` lives in [qrb_ros_interfaces](https://github.com/qualcomm-qrb-ros/qrb_ros_interfaces). Build it from source in the same workspace if the packaged version is older than this repository requires (e.g. a package without `AudioData`).

#### WebRTC processing library selection

The `webrtc_audio_processing_vendor` package downloads the official webrtc-audio-processing 1.3 release tarball (SHA256-pinned) and builds it with meson/ninja; `qrb_audio_common_lib` finds it automatically. Which library ends up linked:

| Situation | Result | Processors available |
| --- | --- | --- |
| meson + ninja + libabsl-dev installed (default) | vendored 1.3 | `aec`, `ns`, `vad` |
| vendor package built nothing (no meson/ninja, or `-DWEBRTC_APM_BUILD=OFF`), system `libwebrtc-audio-processing-dev` installed | system 0.3.1 | `ns`, `vad` |
| neither | none (CMake warning) | only `passthrough`; asking for `aec`/`ns`/`vad` is rejected with a clear log |

Useful CMake options:
- `qrb_audio_common_lib`: `-DQRB_AUDIO_WEBRTC_IMPL=auto|v1|legacy` (force 1.x or 0.3.1; exactly one is linked because they export conflicting symbols), `-DQRB_AUDIO_ENABLE_WEBRTC=OFF` (no WebRTC at all), `-DQRB_AUDIO_WEBRTC1_PREFIX=<prefix>` (use a separately installed 1.x).
- `webrtc_audio_processing_vendor`: `-DWEBRTC_APM_URL=<local tarball>` for offline builds (same SHA256), `-DWEBRTC_APM_BUILD=OFF`.

The node logs what it got at startup, e.g. `capture processing: webrtc-audio-processing 1.x; available processors: passthrough, test_delay, aec, ns, vad`.

### Step 2: Clone and Build

```bash
# Navigate to your ROS2 workspace
cd ~/ros2_ws/src

# Clone the repository (if not already cloned)
git clone https://github.com/qualcomm-qrb-ros/qrb_ros_audio_service.git

# Build (the first build also compiles webrtc-audio-processing, about 35 s on IQ-9075)
cd ~/ros2_ws
colcon build

# Source the workspace
source install/setup.bash
```

## 🤝 Contributing

We love community contributions! Get started by reading our [CONTRIBUTING.md](CONTRIBUTING.md).  
Feel free to create an issue for bug reports, feature requests, or any discussion 💡.

## 📜 License

Project is licensed under the [BSD-3-clause License](https://spdx.org/licenses/BSD-3-Clause.html). See [LICENSE](./LICENSE) for the full license text.

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Voice front-end example: AudioServer with the aec -> ns -> vad capture chain.

Starts the audio service with capture processing enabled and, optionally, the
example client voice_frontend_client.py, which opens one streaming record
stream and prints PCM / VAD statistics (what an ASR or dialog node would
consume).

The processing parameters below are node parameters. They apply to every
streaming record stream (pub_pcm=true) created after the node starts. The
sample rate and channel count are chosen per stream by the client in
AudioRequest.audio_info. The WebRTC processors run on 16-bit PCM at 8, 16, 32
or 48 kHz, with 1 or 2 channels.

Examples:
  # Service only (16 kHz ASR client of your own, default mic, default speaker as echo ref)
  ros2 launch qrb_ros_audio_service voice_frontend.launch.py

  # Service + example client, 16 kHz mono, run for 30 s
  ros2 launch qrb_ros_audio_service voice_frontend.launch.py start_client:=true \\
      sample_rate:=16000 client_duration:=30

  # 48 kHz, explicit devices, stronger NS, more sensitive VAD
  ros2 launch qrb_ros_audio_service voice_frontend.launch.py start_client:=true \\
      sample_rate:=48000 \\
      capture_device:=alsa_input.platform-sound.HiFi__hw_LEMANSEVK_1__source \\
      aec_reference_device:=alsa_output.platform-sound.HiFi__hw_LEMANSEVK_0__sink.monitor \\
      ns_level:=high vad_likelihood:=low

  # NS + VAD only (no echo cancellation, e.g. no speaker on the device)
  ros2 launch qrb_ros_audio_service voice_frontend.launch.py capture_processors:=ns,vad

  # Raw capture, no processing (and therefore no VAD topic)
  ros2 launch qrb_ros_audio_service voice_frontend.launch.py capture_processors:=none
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import ComposableNodeContainer, Node
from launch_ros.descriptions import ComposableNode

# (name, default, description)
_SERVICE_ARGS = [
    ('capture_device', '',
     'Capture source (PipeWire/PulseAudio source name). Empty = default source.'),
    ('playback_device', '', 'Playback sink. Empty = default sink.'),
    ('capture_fragment_duration_ms', '20',
     'Size of each published AudioData chunk in ms (0 = library default 80 ms). '
     'Smaller = lower latency, more messages.'),
    ('capture_processors', 'aec,ns,vad',
     'Comma-separated processing chain, in this order: aec, ns, vad. '
     'Each entry may carry overrides, e.g. "ns:level=high". "none" = no processing.'),
    ('ns_level', 'moderate', 'Noise suppression level: low, moderate, high, very_high.'),
    ('vad_likelihood', 'very_low',
     'VAD aggressiveness: very_low, low, moderate, high (higher = reports voice more readily).'),
    ('vad_hangover_ms', '300', 'Keep "voice" for this long after the last voiced 10 ms block.'),
    ('vad_topic_suffix', '_vad', 'VAD decisions are published on <topic_name><suffix>.'),
    ('aec_reference_device', '@DEFAULT_MONITOR@',
     'Echo reference: monitor source of the sink that drives the speaker. '
     '@DEFAULT_MONITOR@ = monitor of the default sink.'),
]

_CLIENT_ARGS = [
    ('start_client', 'false', 'Also start the example client voice_frontend_client.py.'),
    ('sample_rate', '16000', 'Client stream sample rate: 16000 (ASR) or 48000.'),
    ('channels', '1', 'Client stream channel count (1 or 2).'),
    ('topic_name', 'voice_pcm', 'Client PCM topic (VAD on <topic_name><vad_topic_suffix>).'),
    ('client_duration', '0', 'Client run time in s; 0 = until Ctrl+C.'),
]


def _service(context):
    def val(name):
        return LaunchConfiguration(name).perform(context)

    params = {
        'capture_device': val('capture_device'),
        'playback_device': val('playback_device'),
        'capture_fragment_duration_ms': int(val('capture_fragment_duration_ms')),
        'ns_level': val('ns_level'),
        'vad_likelihood': val('vad_likelihood'),
        'vad_hangover_ms': int(val('vad_hangover_ms')),
        'vad_topic_suffix': val('vad_topic_suffix'),
        'aec_reference_device': val('aec_reference_device'),
    }
    chain_arg = val('capture_processors').strip()
    # "none": ros2 launch cannot take an empty value on the command line.
    chain = [] if chain_arg.lower() == 'none' else         [s.strip() for s in chain_arg.split(',') if s.strip()]
    # An empty list cannot be typed as a string array by launch; leaving the
    # parameter unset gives the node default (empty chain = no processing).
    if chain:
        params['capture_processors'] = chain

    return [ComposableNodeContainer(
        output='screen',
        name='voice_frontend_container',
        namespace='',
        package='rclcpp_components',
        executable='component_container_mt',
        composable_node_descriptions=[
            ComposableNode(
                package='qrb_ros_audio_service',
                plugin='qrb_ros::audio_service::AudioServer',
                parameters=[params],
                extra_arguments=[{'use_intra_process_comms': True}],
            ),
        ],
    )]


def generate_launch_description():
    args = [DeclareLaunchArgument(n, default_value=d, description=h)
            for n, d, h in _SERVICE_ARGS + _CLIENT_ARGS]
    client = Node(
        package='qrb_ros_audio_service',
        executable='voice_frontend_client.py',
        name='voice_frontend_client',
        output='screen',
        condition=IfCondition(LaunchConfiguration('start_client')),
        arguments=[
            '--sample-rate', LaunchConfiguration('sample_rate'),
            '--channels', LaunchConfiguration('channels'),
            '--topic', LaunchConfiguration('topic_name'),
            '--vad-suffix', LaunchConfiguration('vad_topic_suffix'),
            '--duration', LaunchConfiguration('client_duration'),
        ],
    )
    return LaunchDescription(args + [OpaqueFunction(function=_service), client])

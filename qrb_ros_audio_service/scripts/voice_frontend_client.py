#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Example voice front-end client for qrb_ros_audio_service.

Shows the minimal sequence an ASR / dialog node needs:
  1. create a streaming record stream (pub_pcm=true) on /audio_server
  2. start it
  3. consume processed PCM (AudioData) and VAD decisions (std_msgs/Bool)
  4. stop + release on exit

Prints a one-line status every second and every VAD speech start / end.
Optionally writes the received (processed) PCM to a WAV file.

  ros2 run qrb_ros_audio_service voice_frontend_client.py --sample-rate 16000 --duration 30
"""

import argparse
import array
import math
import sys
import time
import wave

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from rclpy.signals import SignalHandlerOptions
from rclpy.utilities import remove_ros_args
from std_msgs.msg import Bool

from qrb_ros_audio_service_msgs.msg import AudioData, AudioInfo
from qrb_ros_audio_service_msgs.srv import AudioRequest


class VoiceFrontendClient(Node):
    def __init__(self, args):
        super().__init__('voice_frontend_client')
        self.args = args
        self.handle = 0
        self.cli = self.create_client(AudioRequest, 'audio_server')
        qos = QoSProfile(depth=50, reliability=ReliabilityPolicy.RELIABLE)
        self.create_subscription(AudioData, args.topic, self.on_pcm, qos)
        self.create_subscription(Bool, args.topic + args.vad_suffix, self.on_vad, qos)
        self.wav = None
        if args.save_wav:
            self.wav = wave.open(args.save_wav, 'wb')
            self.wav.setnchannels(args.channels)
            self.wav.setsampwidth(2)
            self.wav.setframerate(args.sample_rate)
        self.t0 = None
        self.chunks = 0
        self.frames = 0
        self.win_sq = 0.0
        self.win_n = 0
        self.vad_msgs = 0
        self.speech = None
        self.speech_since = 0.0

    # ---- service helpers ----
    def request(self, command, timeout=10.0, **fields):
        req = AudioRequest.Request()
        req.command = command
        for k, v in fields.items():
            setattr(req, k, v)
        fut = self.cli.call_async(req)
        rclpy.spin_until_future_complete(self, fut, timeout_sec=timeout)
        res = fut.result()
        if res is None or not res.success:
            self.get_logger().error(f'"{command}" failed (see the audio service log for the reason)')
            return None
        return res

    def open_stream(self):
        while not self.cli.wait_for_service(timeout_sec=1.0):
            self.get_logger().info('waiting for /audio_server ...')
        res = self.request(
            'create', type='record', pub_pcm=True, topic_name=self.args.topic,
            audio_info=AudioInfo(channels=self.args.channels,
                                 sample_rate=self.args.sample_rate, sample_format=16))
        if res is None:
            return False
        self.handle = res.stream_handle
        if self.request('start', stream_handle=self.handle) is None:
            self.close_stream()
            return False
        self.t0 = time.time()
        self.get_logger().info(
            f'stream {self.handle} started: {self.args.sample_rate} Hz, {self.args.channels} ch, '
            f'PCM on "{self.args.topic}", VAD on "{self.args.topic + self.args.vad_suffix}"')
        self.create_timer(1.0, self.report)
        return True

    def close_stream(self):
        if self.handle:
            # Short timeouts: on Ctrl+C under ros2 launch the service may be exiting too.
            self.request('stop', timeout=3.0, stream_handle=self.handle)
            self.request('release', timeout=3.0, stream_handle=self.handle)
            self.get_logger().info(f'stream {self.handle} released')
            self.handle = 0
        if self.wav:
            self.wav.close()
            self.wav = None

    # ---- data ----
    def on_pcm(self, msg):
        if msg.stream_handle != self.handle:
            return
        samples = array.array('h', bytes(msg.data))
        if sys.byteorder == 'big':
            samples.byteswap()
        self.chunks += 1
        self.frames += len(samples) // self.args.channels
        self.win_sq += sum(s * s for s in samples)
        self.win_n += len(samples)
        if self.wav:
            self.wav.writeframes(bytes(msg.data))
        # Hand msg.data to the ASR engine here (16-bit little-endian interleaved PCM).

    def on_vad(self, msg):
        # One VAD message per PCM chunk, published right after it.
        self.vad_msgs += 1
        now = time.time() - (self.t0 or time.time())
        if self.speech is None or msg.data != self.speech:
            if msg.data:
                self.get_logger().info(f'[{now:7.2f} s] speech START')
            elif self.speech:
                self.get_logger().info(
                    f'[{now:7.2f} s] speech END ({now - self.speech_since:.2f} s)')
            self.speech = msg.data
            self.speech_since = now

    def report(self):
        rms = math.sqrt(self.win_sq / self.win_n) if self.win_n else 0.0
        dbfs = 20 * math.log10(rms / 32768.0) if rms > 0 else -120.0
        self.win_sq, self.win_n = 0.0, 0
        elapsed = time.time() - self.t0
        self.get_logger().info(
            f'{elapsed:6.1f} s: {self.chunks} chunks, {self.frames / self.args.sample_rate:6.1f} s '
            f'audio, level {dbfs:6.1f} dBFS, vad msgs {self.vad_msgs}, '
            f'speech={"yes" if self.speech else "no"}')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sample-rate', type=int, default=16000)
    ap.add_argument('--channels', type=int, default=1)
    ap.add_argument('--topic', default='voice_pcm')
    ap.add_argument('--vad-suffix', default='_vad')
    ap.add_argument('--duration', type=float, default=0, help='seconds; 0 = until Ctrl+C')
    ap.add_argument('--save-wav', default='', help='write the received PCM to this WAV file')
    args = ap.parse_args(remove_ros_args(sys.argv)[1:])

    # Keep the context alive on Ctrl+C so the stream can still be stopped/released.
    rclpy.init(signal_handler_options=SignalHandlerOptions.NO)
    node = VoiceFrontendClient(args)
    rc = 0
    try:
        if not node.open_stream():
            rc = 1
        else:
            end = time.time() + args.duration if args.duration > 0 else None
            while rclpy.ok() and (end is None or time.time() < end):
                rclpy.spin_once(node, timeout_sec=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        node.close_stream()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return rc


if __name__ == '__main__':
    sys.exit(main())

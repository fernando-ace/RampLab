"""Observe and validate the Unreal RGB Image/CameraInfo stream over ROS 2."""
import argparse
import math
import time

import rclpy
from rclpy.qos import QoSProfile, QoSReliabilityPolicy
from rclpy.node import Node
from sensor_msgs.msg import CameraInfo, Image


class CameraProbe(Node):
    def __init__(self, target_frames: int):
        super().__init__("ramplab_camera_transport_probe", namespace="/ramplab/tug1")
        self.target_frames = target_frames
        self.images = {}
        self.infos = {}
        self.valid_pairs = 0
        self.last_stamp_ns = -1
        sensor_qos = QoSProfile(depth=10, reliability=QoSReliabilityPolicy.BEST_EFFORT)
        self.create_subscription(Image, "camera/image_raw", self.on_image, sensor_qos)
        self.create_subscription(CameraInfo, "camera/camera_info", self.on_info, sensor_qos)

    @staticmethod
    def stamp_ns(message):
        return message.header.stamp.sec * 1_000_000_000 + message.header.stamp.nanosec

    def on_image(self, message: Image):
        stamp = self.stamp_ns(message)
        if message.header.frame_id != "camera" or message.encoding != "bgra8":
            raise RuntimeError("unexpected camera image frame or encoding")
        if message.width == 0 or message.height == 0 or message.step != message.width * 4:
            raise RuntimeError("invalid camera image dimensions or row stride")
        if len(message.data) != message.step * message.height:
            raise RuntimeError("camera image byte count does not match dimensions")
        if not any(message.data[offset] or message.data[offset + 1] or message.data[offset + 2]
                   for offset in range(0, len(message.data), 4)):
            raise RuntimeError("camera image contains no RGB scene pixels")
        if stamp <= self.last_stamp_ns:
            raise RuntimeError("camera simulation timestamps did not increase")
        self.last_stamp_ns = stamp
        self.images[stamp] = message
        self.match(stamp)

    def on_info(self, message: CameraInfo):
        stamp = self.stamp_ns(message)
        self.infos[stamp] = message
        self.match(stamp)

    def match(self, stamp):
        image = self.images.get(stamp)
        info = self.infos.get(stamp)
        if image is None or info is None:
            return
        if info.header.frame_id != image.header.frame_id or (info.width, info.height) != (image.width, image.height):
            raise RuntimeError("CameraInfo does not describe its paired image")
        if len(info.k) != 9 or not all(math.isfinite(value) for value in info.k) or info.k[0] <= 0 or info.k[4] <= 0:
            raise RuntimeError("CameraInfo intrinsics are invalid")
        self.valid_pairs += 1
        self.images.pop(stamp, None)
        self.infos.pop(stamp, None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=45.0)
    args = parser.parse_args()
    if args.frames < 1 or args.timeout <= 0:
        parser.error("frames and timeout must be positive")
    rclpy.init()
    node = CameraProbe(args.frames)
    deadline = time.monotonic() + args.timeout
    try:
        while node.valid_pairs < args.frames and time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
        if node.valid_pairs < args.frames:
            raise RuntimeError(f"received {node.valid_pairs} valid image/info pairs; expected {args.frames}")
        print(f"Camera transport PASS: valid_image_info_pairs={node.valid_pairs} last_stamp_ns={node.last_stamp_ns}")
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()

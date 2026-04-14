#!/usr/bin/env python3
"""
UDP Pixel Sender for ESP32 HUB75 64x64 Display

Usage:
  python3 send-video.py video.mp4           # play video
  python3 send-video.py animation.gif       # play GIF (loops)
  python3 send-video.py photo.png           # show image for 5s
  python3 send-video.py --fps 25 video.mp4  # override FPS
  python3 send-video.py --loop video.mp4    # loop video

  # Pipe raw RGB from ffmpeg:
  ffmpeg -i input.mp4 -vf scale=64:64 -pix_fmt rgb24 -f rawvideo pipe:1 | \\
    python3 send-video.py --raw --fps 25

  # Screen capture:
  ffmpeg -f x11grab -framerate 10 -video_size 1920x1080 -i :0 \\
    -vf scale=64:64 -pix_fmt rgb24 -f rawvideo pipe:1 | \\
    python3 send-video.py --raw --fps 10
"""

import argparse
import socket
import struct
import sys
import time
PANEL_W = 64
PANEL_H = 64
FRAME_SIZE = PANEL_W * PANEL_H * 3  # 12288 bytes
DEFAULT_HOST = "192.168.199.144"
DEFAULT_PORT = 5005
CHUNK_DATA = 1400  # payload per UDP packet (safe under MTU)
HEADER_SIZE = 4
ACK_TIMEOUT = 0.5  # seconds to wait for ACK


def send_frame(sock, addr, frame_bytes, frame_id):
    """Send one frame as chunked UDP packets."""
    num_chunks = (FRAME_SIZE + CHUNK_DATA - 1) // CHUNK_DATA

    for chunk_idx in range(num_chunks):
        offset = chunk_idx * CHUNK_DATA
        end = min(offset + CHUNK_DATA, FRAME_SIZE)
        header = bytes([frame_id & 0xFF, chunk_idx, num_chunks, 0])
        sock.sendto(header + frame_bytes[offset:end], addr)
    # No ACK wait — sender paces itself via FPS timing


def send_raw_stdin(sock, addr, target_fps):
    """Read raw RGB888 frames from stdin (piped from ffmpeg)."""
    frame_time = 1.0 / target_fps
    frame_id = 0
    count = 0
    t_start = time.monotonic()

    while True:
        data = sys.stdin.buffer.read(FRAME_SIZE)
        if len(data) < FRAME_SIZE:
            break

        t0 = time.monotonic()
        acked = send_frame(sock, addr, data, frame_id)
        frame_id = (frame_id + 1) & 0xFF
        count += 1

        elapsed = time.monotonic() - t0
        if frame_time > elapsed:
            time.sleep(frame_time - elapsed)

        if count % 50 == 0:
            actual_fps = count / (time.monotonic() - t_start) if count > 0 else 0
            print(f"\r  Sent {count} frames ({actual_fps:.1f} fps)", end="", flush=True)

    print(f"\n  Done: {count} frames")


def send_image_file(sock, addr, path, duration=5.0):
    """Send a static image, display for duration seconds."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: pip install pillow")
        sys.exit(1)

    img = Image.open(path).convert("RGB").resize((PANEL_W, PANEL_H), Image.LANCZOS)
    frame_bytes = img.tobytes()
    print(f"  Showing {path} for {duration}s")
    send_frame(sock, addr, frame_bytes, 0)
    time.sleep(duration)


def send_video_file(sock, addr, path, target_fps=0, loop=False):
    """Send a video file using OpenCV."""
    try:
        import cv2
    except ImportError:
        print("ERROR: pip install opencv-python-headless")
        sys.exit(1)

    while True:
        cap = cv2.VideoCapture(path)
        if not cap.isOpened():
            print(f"ERROR: Cannot open {path}")
            sys.exit(1)

        src_fps = cap.get(cv2.CAP_PROP_FPS) or 25
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = target_fps if target_fps > 0 else min(src_fps, 30)
        frame_time = 1.0 / fps

        print(f"  {path}: {total} frames, {src_fps:.0f}fps source, {fps:.0f}fps target")

        frame_id = 0
        count = 0
        t_start = time.monotonic()

        while True:
            t0 = time.monotonic()
            ret, frame = cap.read()
            if not ret:
                break

            frame = cv2.resize(frame, (PANEL_W, PANEL_H), interpolation=cv2.INTER_AREA)
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            send_frame(sock, addr, frame.tobytes(), frame_id)
            frame_id = (frame_id + 1) & 0xFF
            count += 1

            elapsed = time.monotonic() - t0
            if frame_time > elapsed:
                time.sleep(frame_time - elapsed)

            if count % 30 == 0:
                actual_fps = count / (time.monotonic() - t_start) if count > 0 else 0
                print(f"\r  Frame {count}/{total} ({actual_fps:.1f} fps)", end="", flush=True)

        cap.release()
        actual_fps = count / (time.monotonic() - t_start) if count > 0 else 0
        print(f"\n  Done: {count} frames ({actual_fps:.1f} fps avg)")

        if not loop:
            break
        print("  Looping...")


def send_gif_file(sock, addr, path, target_fps=0):
    """Send a GIF animation using Pillow, loops forever."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: pip install pillow")
        sys.exit(1)

    img = Image.open(path)
    frames = []
    durations = []
    try:
        while True:
            frame = img.convert("RGB").resize((PANEL_W, PANEL_H), Image.LANCZOS)
            frames.append(frame.tobytes())
            durations.append(img.info.get("duration", 100) / 1000.0)
            img.seek(img.tell() + 1)
    except EOFError:
        pass

    print(f"  GIF: {len(frames)} frames, looping (Ctrl+C to stop)")
    frame_id = 0
    loop_count = 0

    while True:
        for frame_bytes, duration in zip(frames, durations):
            t0 = time.monotonic()
            if target_fps > 0:
                duration = 1.0 / target_fps
            send_frame(sock, addr, frame_bytes, frame_id)
            frame_id = (frame_id + 1) & 0xFF
            elapsed = time.monotonic() - t0
            if duration > elapsed:
                time.sleep(duration - elapsed)

        loop_count += 1
        if loop_count % 10 == 0:
            print(f"\r  Loop {loop_count} ({len(frames)} frames each)", end="", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Send video/images to ESP32 64x64 LED matrix")
    parser.add_argument("input", nargs="?", help="Video/image/GIF file")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP32 IP (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default: {DEFAULT_PORT})")
    parser.add_argument("--fps", type=float, default=0, help="Target FPS (0 = use source)")
    parser.add_argument("--raw", action="store_true", help="Read raw RGB888 from stdin")
    parser.add_argument("--loop", action="store_true", help="Loop video file")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    addr = (args.host, args.port)
    print(f"Target: {args.host}:{args.port} ({PANEL_W}x{PANEL_H} RGB888, {FRAME_SIZE} bytes/frame)")

    try:
        if args.raw:
            send_raw_stdin(sock, addr, args.fps or 25)
        elif args.input:
            ext = args.input.lower().rsplit(".", 1)[-1]
            if ext == "gif":
                send_gif_file(sock, addr, args.input, args.fps)
            elif ext in ("png", "jpg", "jpeg", "bmp", "webp"):
                send_image_file(sock, addr, args.input)
            else:
                send_video_file(sock, addr, args.input, args.fps, args.loop)
        else:
            parser.print_help()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()

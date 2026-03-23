#!/usr/bin/env python3
"""
Serial control interface for ML data collection on ESP32-S3.

Sends commands to the firmware's serial command parser to control
data collection sessions, export data, and monitor status.

Commands:
    ML_START <label>     Start recording session with given gesture label
    ML_STOP              Stop current recording session
    ML_STATUS            Print logger and inference status
    ML_DELETE             Delete all ML data from SPIFFS
    ML_EXPORT             Dump raw session data over serial (binary)

Supported labels:
    idle, approach, hover, quick_pass, cover_hold

Usage:
    python serial_control.py /dev/ttyACM0 start idle
    python serial_control.py /dev/ttyACM0 stop
    python serial_control.py /dev/ttyACM0 status
    python serial_control.py /dev/ttyACM0 delete
    python serial_control.py COM3 start hover

Interactive mode:
    python serial_control.py /dev/ttyACM0 --interactive

Requirements:
    pip install pyserial
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


VALID_LABELS = [
    "idle", "approach", "hover", "quick_pass", "cover_hold",
    "confirmed_event", "surface_reflection", "noise", "unknown",
]
BAUD_RATE = 115200


def send_command(ser: serial.Serial, cmd: str, wait_ms: int = 500):
    """Send a command and print response lines."""
    ser.write(f"{cmd}\n".encode())
    ser.flush()

    # Read response lines
    time.sleep(wait_ms / 1000.0)
    while ser.in_waiting:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(f"  < {line}")


def cmd_start(ser: serial.Serial, label: str):
    """Start a labeled recording session."""
    if label not in VALID_LABELS:
        print(f"Invalid label: {label}")
        print(f"Valid labels: {', '.join(VALID_LABELS)}")
        return
    print(f"Starting session: label={label}")
    send_command(ser, f"ML_START {label}")


def cmd_stop(ser: serial.Serial):
    """Stop the current session."""
    print("Stopping session...")
    send_command(ser, "ML_STOP")


def cmd_status(ser: serial.Serial):
    """Request status report."""
    send_command(ser, "ML_STATUS", wait_ms=1000)


def cmd_delete(ser: serial.Serial):
    """Delete all ML data."""
    confirm = input("Delete ALL ML data? (yes/no): ")
    if confirm.lower() != "yes":
        print("Cancelled.")
        return
    print("Deleting all ML data...")
    send_command(ser, "ML_DELETE", wait_ms=2000)


def interactive_mode(ser: serial.Serial):
    """Interactive command loop."""
    print("\nML Data Collection - Interactive Mode")
    print("Commands: start <label>, stop, status, delete, quit")
    print(f"Labels: {', '.join(VALID_LABELS)}")
    print()

    while True:
        try:
            cmd = input("ml> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nExiting.")
            break

        if not cmd:
            continue

        parts = cmd.split()
        verb = parts[0].lower()

        if verb == "quit" or verb == "exit":
            break
        elif verb == "start" and len(parts) >= 2:
            cmd_start(ser, parts[1])
        elif verb == "stop":
            cmd_stop(ser)
        elif verb == "status":
            cmd_status(ser)
        elif verb == "delete":
            cmd_delete(ser)
        elif verb == "help":
            print("Commands: start <label>, stop, status, delete, quit")
            print(f"Labels: {', '.join(VALID_LABELS)}")
        else:
            print(f"Unknown command: {cmd}")
            print("Type 'help' for available commands")


def main():
    parser = argparse.ArgumentParser(
        description="Serial control for ML data collection")
    parser.add_argument("port", help="Serial port (e.g., /dev/ttyACM0 or COM3)")
    parser.add_argument("command", nargs="?", default=None,
                        choices=["start", "stop", "status", "delete"],
                        help="Command to send")
    parser.add_argument("label", nargs="?", default=None,
                        help="Gesture label (for 'start' command)")
    parser.add_argument("--interactive", "-i", action="store_true",
                        help="Enter interactive mode")
    parser.add_argument("--baud", type=int, default=BAUD_RATE,
                        help=f"Baud rate (default: {BAUD_RATE})")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        time.sleep(0.5)  # Wait for serial to stabilize
    except serial.SerialException as e:
        print(f"Failed to open {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.interactive or args.command is None:
            interactive_mode(ser)
        elif args.command == "start":
            if not args.label:
                print("Usage: serial_control.py PORT start <label>")
                sys.exit(1)
            cmd_start(ser, args.label)
        elif args.command == "stop":
            cmd_stop(ser)
        elif args.command == "status":
            cmd_status(ser)
        elif args.command == "delete":
            cmd_delete(ser)
    finally:
        ser.close()


if __name__ == "__main__":
    main()

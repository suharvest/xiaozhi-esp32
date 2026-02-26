#!/usr/bin/env python3
"""Test script to verify UDP beacon reception from ESP32.
Run on the same network as the ESP32 device.

Usage:
    python test_beacon_listener.py          # Listen for real beacons
    python test_beacon_listener.py --send   # Send a fake beacon (for testing RPi listener)
"""
import socket
import sys
import time

BEACON_PORT = 12321

def listen():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(('', BEACON_PORT))
    sock.settimeout(10)
    print(f"Listening for UDP beacons on port {BEACON_PORT} (10s timeout)...")
    count = 0
    try:
        while True:
            data, addr = sock.recvfrom(256)
            count += 1
            text = data.decode('utf-8', errors='ignore')
            print(f"  [{count}] From {addr[0]}:{addr[1]} -> {text}")
    except socket.timeout:
        if count == 0:
            print("No beacons received in 10 seconds!")
            print("Possible issues:")
            print("  - ESP32 beacon not started (check serial log for 'UDP beacon started')")
            print("  - Firewall blocking UDP port 12321")
            print("  - Devices on different subnets/VLANs")
        else:
            print(f"Received {count} beacon(s)")
    finally:
        sock.close()

def send_fake():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = "XZWATCH|Test Device|192.168.1.99|80|test-board|1.0"
    dest = ('255.255.255.255', BEACON_PORT)
    for i in range(5):
        sock.sendto(msg.encode(), dest)
        print(f"Sent fake beacon #{i+1}: {msg}")
        time.sleep(2)
    sock.close()

if __name__ == '__main__':
    if '--send' in sys.argv:
        send_fake()
    else:
        listen()

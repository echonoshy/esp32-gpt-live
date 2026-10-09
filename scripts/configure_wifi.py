"""Provision Wi-Fi over USB without credentials in source or command arguments."""
import argparse
import getpass
import json
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="/dev/cu.usbmodem1101")
args = parser.parse_args()
ssid = input("2.4 GHz Wi-Fi name: ")
password = getpass.getpass("Wi-Fi password: ")
if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 64:
    raise SystemExit("Invalid Wi-Fi name/password length")
port = serial.Serial(port=None, baudrate=115200, timeout=.2)
port.port = args.port
port.dtr = False
port.rts = False
port.open()
try:
    time.sleep(2)
    port.reset_input_buffer()
    command = "wifi " + json.dumps({"ssid":ssid,"password":password}, ensure_ascii=False) + "\n"
    port.write(command.encode())
    del command, password
    deadline = time.monotonic() + 10
    buffer = bytearray()
    while time.monotonic() < deadline:
        buffer.extend(port.read(1024))
        if b"WIFI_CONFIG_SAVED" in buffer:
            print("Wi-Fi saved on device. It is restarting.")
            break
        if b"WIFI_CONFIG_INVALID" in buffer:
            raise SystemExit("Device rejected Wi-Fi settings")
    else:
        raise SystemExit("No acknowledgement. Check the USB port and firmware.")
finally:
    port.close()

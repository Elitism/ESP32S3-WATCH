# screen_streamer.py (Corrected Version)
#
# This Python script acts as a WebSocket server that captures the desktop,
# processes the image, and sends it to the ESP32-S3-Touch-AMOLED-2.06 display.
#
# It automatically installs dependencies and is designed to work with the
# provided Arduino sketch.
#
# Python Version: 3.7+
#
# How to run:
# 1. Save this file as "screen_streamer.py".
# 2. Open a terminal or command prompt.
# 3. Run the script using: python screen_streamer.py
# 4. The script will print your PC's IP address. Update the `serverIp`
#    variable in your Arduino sketch with this IP.
# 5. Upload the sketch to your ESP32 and power it on.

import sys
import subprocess
import importlib

# --- Configuration ---
DISPLAY_WIDTH = 410
DISPLAY_HEIGHT = 502
WEBSOCKET_PORT = 81
TARGET_FPS = 20 # You can adjust this for performance

# --- Dependency Management ---
def install_dependencies():
    """Checks for required packages and installs them if missing."""
    packages = {
        'websockets': 'websockets',
        'mss': 'mss',
        'PIL': 'Pillow',
        'numpy': 'numpy'
    }
    print("Checking for required Python packages...")
    for module_name, package_name in packages.items():
        try:
            importlib.import_module(module_name)
            print(f"  [✓] {package_name} is already installed.")
        except ImportError:
            print(f"  [!] {package_name} not found. Attempting to install...")
            try:
                subprocess.check_call([sys.executable, "-m", "pip", "install", package_name])
                print(f"  [✓] Successfully installed {package_name}.")
            except subprocess.CalledProcessError as e:
                print(f"  [✗] Failed to install {package_name}. Please install it manually using 'pip install {package_name}'")
                print(f"  Error: {e}")
                sys.exit(1)
    print("-" * 30)

# Run dependency check first
install_dependencies()

# --- Main Imports (after installation) ---
import asyncio
import websockets
import numpy as np
import socket
from mss import mss
from PIL import Image

# --- Image Processing ---
def capture_and_process_frame(sct):
    """Captures, rotates, resizes, and converts a screen frame to RGB565."""
    # 1. Capture the primary monitor
    # sct.monitors[0] is all monitors, sct.monitors[1] is the primary one.
    monitor = sct.monitors[1]
    sct_img = sct.grab(monitor)

    # 2. Convert to PIL Image
    # The screen grab is in BGRA format, convert it to RGB
    img = Image.frombytes("RGB", sct_img.size, sct_img.bgra, "raw", "BGRX")

    # 3. Rotate 90 degrees
    # 'expand=True' ensures the new dimensions are set correctly after rotation
    img_rotated = img.rotate(90, expand=True)

    # 4. Resize to fit the device's display
    img_resized = img_rotated.resize((DISPLAY_WIDTH, DISPLAY_HEIGHT))

    # 5. Convert to RGB565 format using NumPy for high performance
    # Convert the PIL image to a NumPy array of 8-bit unsigned integers
    np_arr = np.array(img_resized, dtype=np.uint8)

    # Extract Red, Green, and Blue channels, casting to 16-bit to prevent overflow
    r = np_arr[:, :, 0].astype(np.uint16)
    g = np_arr[:, :, 1].astype(np.uint16)
    b = np_arr[:, :, 2].astype(np.uint16)

    # Perform the bitwise operations to pack RGB888 into a single 16-bit value (RGB565)
    # R (5 bits): (r & 0xF8) << 8
    # G (6 bits): (g & 0xFC) << 3
    # B (5 bits): b >> 3
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    # Return the raw bytes of the 16-bit NumPy array
    return rgb565.tobytes()

# --- WebSocket Server Logic ---
async def handler(websocket): # <<< THIS IS THE CORRECTED LINE
    """Handles a single client connection, streaming screen frames."""
    print(f"🟢 Client connected from {websocket.remote_address}")
    # Create a screen capture object once per connection
    with mss() as sct:
        try:
            while True:
                frame_data = capture_and_process_frame(sct)
                await websocket.send(frame_data)
                # Control the frame rate to avoid overwhelming the ESP32
                await asyncio.sleep(1 / TARGET_FPS)
        except websockets.exceptions.ConnectionClosed as e:
            print(f"🔴 Client disconnected: {e.reason} (Code: {e.code})")
        except Exception as e:
            print(f"An unexpected error occurred: {e}")
        finally:
            print("Connection closed.")

# --- Main Function ---
async def main():
    """Finds the local IP and starts the WebSocket server."""
    # Find the local IP address of the PC
    hostname = socket.gethostname()
    try:
        ip_address = socket.gethostbyname(hostname)
        # A more robust way to find the LAN IP
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80)) # Connect to a public DNS to find the route
        ip_address = s.getsockname()[0]
        s.close()
    except Exception:
        ip_address = "127.0.0.1" # Fallback

    # Start the server
    async with websockets.serve(handler, "0.0.0.0", WEBSOCKET_PORT, max_size=None):
        print("======================================================")
        print("   🚀 WebSocket Screen Streamer is running! 🚀")
        print("======================================================")
        print(f"Please update your Arduino sketch with this IP address:")
        print(f"-> String serverIp = \"{ip_address}\";")
        print("------------------------------------------------------")
        print(f"Listening on: ws://{ip_address}:{WEBSOCKET_PORT}")
        print("Waiting for a client (your ESP32) to connect...")
        print("Press Ctrl+C to stop the server.")
        
        # Keep the server running forever
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer is shutting down. Goodbye! 👋")
#!/usr/bin/env python3
"""Send a single command to Bowie Phone and show output for a few seconds"""
import serial
import sys
import time

def main():
    if len(sys.argv) < 2:
        print("Usage: python send_command.py <command> [port] [wait_seconds]")
        print("Example: python send_command.py hook COM3 3")
        return 1
    
    command = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else "COM3"
    wait_time = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    
    try:
        # Disable DTR/RTS to prevent ESP32 reset on connect
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.1
        ser.dtr = False  # Disable DTR
        ser.rts = False  # Disable RTS
        ser.open()
        
        print(f"Connected to {port} (DTR/RTS disabled)")
        
        # Small delay to let any pending data settle
        time.sleep(0.1)
        
        # Flush any boot messages
        while ser.in_waiting:
            ser.read(ser.in_waiting)
        
        # Send command
        ser.write((command + '\n').encode('utf-8'))
        ser.flush()
        print(f">>> Sent: {command}")
        print("-" * 50)
        
        # Read output for wait_time seconds
        start = time.time()
        while time.time() - start < wait_time:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').rstrip()
                if line:
                    print(f"<<< {line}")
            time.sleep(0.01)
        
        print("-" * 50)
        ser.close()
        return 0
        
    except Exception as e:
        print(f"Error: {e}")
        return 1

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""
Bowie Phone Serial Tester - Interactive Debug Mode
===================================================
Collaborative debugging tool for Bowie Phone via serial connection.

Usage:
    python serial_test.py [COM_PORT] [BAUD_RATE]
"""

import serial
import serial.tools.list_ports
import threading
import sys
import time
import os
import re
from collections import deque
from datetime import datetime

class BowiePhoneTester:
    def __init__(self, port=None, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.serial = None
        self.running = False
        self.reader_thread = None
        self.output_buffer = deque(maxlen=100)  # Keep last 100 lines
        self.last_state = {
            'hook': None,
            'audio_playing': False,
            'last_sequence': None,
            'errors': [],
            'warnings': []
        }
        self.output_lock = threading.Lock()
        
    def find_port(self):
        """Auto-detect ESP32 serial port"""
        ports = list(serial.tools.list_ports.comports())
        esp_keywords = ['CP210', 'CH340', 'FTDI', 'USB-SERIAL', 'Silicon Labs', 'USB2.0-Serial']
        
        for port in ports:
            desc = f"{port.description} {port.manufacturer or ''}".upper()
            for keyword in esp_keywords:
                if keyword.upper() in desc:
                    return port.device
        
        if ports:
            return ports[0].device
        return None
    
    def connect(self):
        """Connect to the device"""
        if not self.port:
            self.port = self.find_port()
            
        if not self.port:
            print("❌ No serial port found. Please specify a port.")
            return False
        
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=0.1,
                write_timeout=1
            )
            print(f"✅ Connected to {self.port} at {self.baudrate} baud")
            return True
        except serial.SerialException as e:
            print(f"❌ Failed to connect to {self.port}: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from the device"""
        self.running = False
        if self.reader_thread:
            self.reader_thread.join(timeout=1)
        if self.serial and self.serial.is_open:
            self.serial.close()
            print("🔌 Disconnected")
    
    def parse_output(self, text):
        """Parse device output and track state"""
        with self.output_lock:
            # Track hook state
            if 'OFF HOOK' in text:
                self.last_state['hook'] = 'OFF_HOOK'
            elif 'ON HOOK' in text:
                self.last_state['hook'] = 'ON_HOOK'
            
            # Track audio
            if 'Playing' in text or 'dialtone' in text.lower():
                self.last_state['audio_playing'] = True
            if 'Stopped' in text or 'stopAudio' in text:
                self.last_state['audio_playing'] = False
            
            # Track sequences
            seq_match = re.search(r"sequence[:\s]+['\"]?(\d+)['\"]?", text, re.IGNORECASE)
            if seq_match:
                self.last_state['last_sequence'] = seq_match.group(1)
            
            # Track errors
            if '❌' in text or 'Error' in text.upper() or 'Failed' in text:
                self.last_state['errors'].append((datetime.now(), text))
            
            # Track warnings
            if '⚠️' in text or 'Warning' in text:
                self.last_state['warnings'].append((datetime.now(), text))
    
    def read_output(self):
        """Background thread to read and display serial output"""
        while self.running:
            try:
                if self.serial.in_waiting:
                    line = self.serial.readline()
                    try:
                        text = line.decode('utf-8', errors='replace').rstrip()
                        if text:
                            timestamp = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                            self.output_buffer.append((timestamp, text))
                            self.parse_output(text)
                            
                            # Color-code output
                            if '❌' in text or 'Error' in text:
                                print(f"\033[91m[{timestamp}] {text}\033[0m")  # Red
                            elif '⚠️' in text or 'Warning' in text:
                                print(f"\033[93m[{timestamp}] {text}\033[0m")  # Yellow
                            elif '✅' in text:
                                print(f"\033[92m[{timestamp}] {text}\033[0m")  # Green
                            elif 'DTMF' in text or 'sequence' in text.lower():
                                print(f"\033[96m[{timestamp}] {text}\033[0m")  # Cyan
                            elif 'Playing' in text or 'audio' in text.lower():
                                print(f"\033[95m[{timestamp}] {text}\033[0m")  # Magenta
                            else:
                                print(f"[{timestamp}] {text}")
                    except:
                        print(f"[raw] {line}")
            except serial.SerialException:
                break
            except Exception as e:
                if self.running:
                    print(f"⚠️ Read error: {e}")
                break
            time.sleep(0.01)
    
    def send_command(self, cmd):
        """Send a command to the device"""
        if not self.serial or not self.serial.is_open:
            print("❌ Not connected")
            return False
        
        try:
            cmd_bytes = (cmd + '\n').encode('utf-8')
            self.serial.write(cmd_bytes)
            self.serial.flush()
            print(f"\n{'='*50}")
            print(f"📤 SENT: {cmd}")
            print(f"{'='*50}")
            return True
        except serial.SerialException as e:
            print(f"❌ Send failed: {e}")
            return False
    
    def wait_and_confirm(self, prompt, timeout=5):
        """Wait for output and ask user for confirmation"""
        print(f"\n⏳ Waiting {timeout}s for device response...")
        time.sleep(timeout)
        
        response = input(f"\n❓ {prompt} (y/n/s=skip): ").strip().lower()
        return response
    
    def show_state(self):
        """Show current tracked state"""
        print("\n" + "="*50)
        print("📊 CURRENT DEVICE STATE")
        print("="*50)
        print(f"   Hook State: {self.last_state['hook'] or 'Unknown'}")
        print(f"   Audio Playing: {self.last_state['audio_playing']}")
        print(f"   Last Sequence: {self.last_state['last_sequence'] or 'None'}")
        print(f"   Errors: {len(self.last_state['errors'])}")
        print(f"   Warnings: {len(self.last_state['warnings'])}")
        print("="*50 + "\n")
    
    def show_errors(self):
        """Show all recorded errors"""
        if not self.last_state['errors']:
            print("✅ No errors recorded")
            return
        print("\n🔴 ERRORS:")
        for ts, err in self.last_state['errors']:
            print(f"   [{ts.strftime('%H:%M:%S')}] {err}")
    
    def show_warnings(self):
        """Show all recorded warnings"""
        if not self.last_state['warnings']:
            print("✅ No warnings recorded")
            return
        print("\n🟡 WARNINGS:")
        for ts, warn in self.last_state['warnings']:
            print(f"   [{ts.strftime('%H:%M:%S')}] {warn}")
    
    def run_interactive_test(self):
        """Run interactive debug test sequence"""
        print("\n" + "="*60)
        print("🧪 INTERACTIVE DEBUG TEST")
        print("="*60)
        print("We'll test each step together. I'll send commands and")
        print("you confirm what you observe (sound, behavior, etc.)")
        print("="*60)
        
        input("\n🔹 Press ENTER when ready to begin...")
        
        # Step 1: Check initial state
        print("\n" + "-"*50)
        print("STEP 1: Initial State Check")
        print("-"*50)
        self.show_state()
        input("Press ENTER to continue...")
        
        # Step 2: Set log level to debug
        print("\n" + "-"*50)
        print("STEP 2: Enable Debug Logging")
        print("-"*50)
        self.send_command("level 2")
        time.sleep(1)
        print("✅ Debug logging enabled - we'll see more details now")
        input("Press ENTER to continue...")
        
        # Step 3: Toggle hook to OFF HOOK
        print("\n" + "-"*50)
        print("STEP 3: Toggle Hook to OFF HOOK")
        print("-"*50)
        print("Expected: Phone goes off-hook, dial tone should play")
        self.send_command("hook")
        
        response = self.wait_and_confirm("Do you hear a DIAL TONE?", timeout=3)
        if response == 'y':
            print("✅ Dial tone confirmed!")
        elif response == 'n':
            print("⚠️ No dial tone - this may indicate an audio issue")
            self.last_state['errors'].append((datetime.now(), "USER: No dial tone heard"))
        
        self.show_state()
        input("Press ENTER to continue...")
        
        # Step 4: Send sequence 6969
        print("\n" + "-"*50)
        print("STEP 4: Send DTMF Sequence '6969'")
        print("-"*50)
        print("Expected: Dial tone stops, sequence processed, audio plays")
        self.send_command("6969")
        
        response = self.wait_and_confirm("Did the dial tone STOP?", timeout=2)
        if response == 'y':
            print("✅ Dial tone stopped on digit input")
        elif response == 'n':
            print("⚠️ Dial tone should stop when digits are entered")
        
        response = self.wait_and_confirm("Do you hear audio playing for sequence 6969?", timeout=5)
        if response == 'y':
            print("✅ Audio playing for 6969!")
        elif response == 'n':
            print("⚠️ No audio for 6969 - may not be in catalog or playback issue")
            self.last_state['errors'].append((datetime.now(), "USER: No audio for 6969"))
        
        self.show_state()
        input("Press ENTER to continue...")
        
        # Step 5: Send sequence 888
        print("\n" + "-"*50)
        print("STEP 5: Send DTMF Sequence '888'")
        print("-"*50)
        print("Expected: Previous audio stops (if any), new sequence processed")
        self.send_command("888")
        
        response = self.wait_and_confirm("Do you hear audio playing for sequence 888?", timeout=5)
        if response == 'y':
            print("✅ Audio playing for 888!")
        elif response == 'n':
            print("⚠️ No audio for 888 - may not be in catalog")
            self.last_state['errors'].append((datetime.now(), "USER: No audio for 888"))
        
        self.show_state()
        input("Press ENTER to continue...")
        
        # Step 6: Toggle hook to ON HOOK
        print("\n" + "-"*50)
        print("STEP 6: Toggle Hook to ON HOOK (hang up)")
        print("-"*50)
        print("Expected: Audio stops, phone goes on-hook")
        self.send_command("hook")
        
        response = self.wait_and_confirm("Did the audio STOP?", timeout=2)
        if response == 'y':
            print("✅ Audio stopped on hangup")
        elif response == 'n':
            print("⚠️ Audio should stop when phone hangs up")
        
        self.show_state()
        
        # Summary
        print("\n" + "="*60)
        print("📋 TEST SUMMARY")
        print("="*60)
        self.show_errors()
        self.show_warnings()
        print("\n" + "="*60)
        
        input("\nPress ENTER to return to interactive mode...")
    
    def show_help(self):
        """Show help message"""
        print("""
╔══════════════════════════════════════════════════════════════╗
║         Bowie Phone Interactive Debugger - Commands          ║
╠══════════════════════════════════════════════════════════════╣
║  DEVICE COMMANDS:                                            ║
║    hook          Toggle hook state (on/off hook)             ║
║    <digits>      Send DTMF sequence (e.g., 6969, 888, 911)   ║
║    level <n>     Set log level (0=quiet, 1=normal, 2=debug)  ║
║    <freq>hz      Test FFT with frequency (e.g., 697hz)       ║
║                                                              ║
║  DEBUG COMMANDS:                                             ║
║    test          Run interactive test sequence               ║
║    state         Show current tracked device state           ║
║    errors        Show all recorded errors                    ║
║    warnings      Show all recorded warnings                  ║
║    clear         Clear screen                                ║
║    help          Show this help                              ║
║    quit/exit     Exit the program                            ║
╚══════════════════════════════════════════════════════════════╝
""")
    
    def interactive_mode(self):
        """Run interactive command mode"""
        self.running = True
        self.reader_thread = threading.Thread(target=self.read_output, daemon=True)
        self.reader_thread.start()
        
        print("\n" + "="*60)
        print("🎛️  BOWIE PHONE INTERACTIVE DEBUGGER")
        print("="*60)
        print("Type 'test' to run interactive debug sequence")
        print("Type 'help' for all commands")
        print("="*60 + "\n")
        
        while self.running:
            try:
                cmd = input("\nbowie> ").strip()
                
                if not cmd:
                    continue
                
                cmd_lower = cmd.lower()
                
                if cmd_lower in ('quit', 'exit', 'q'):
                    break
                elif cmd_lower == 'help':
                    self.show_help()
                elif cmd_lower == 'clear':
                    os.system('cls' if os.name == 'nt' else 'clear')
                elif cmd_lower == 'test':
                    self.run_interactive_test()
                elif cmd_lower == 'state':
                    self.show_state()
                elif cmd_lower == 'errors':
                    self.show_errors()
                elif cmd_lower == 'warnings':
                    self.show_warnings()
                else:
                    self.send_command(cmd)
                    
            except KeyboardInterrupt:
                print("\n👋 Interrupted")
                break
            except EOFError:
                break
        
        self.disconnect()


def main():
    port = None
    baudrate = 115200
    
    if len(sys.argv) > 1:
        port = sys.argv[1]
    if len(sys.argv) > 2:
        try:
            baudrate = int(sys.argv[2])
        except ValueError:
            print(f"⚠️ Invalid baud rate: {sys.argv[2]}, using 115200")
    
    print("\n🔍 Scanning for serial ports...")
    ports = list(serial.tools.list_ports.comports())
    if ports:
        print("   Available ports:")
        for p in ports:
            print(f"   - {p.device}: {p.description}")
    else:
        print("   No serial ports found!")
        return 1
    
    tester = BowiePhoneTester(port=port, baudrate=baudrate)
    
    if not tester.connect():
        return 1
    
    print("⏳ Waiting for device to initialize...")
    time.sleep(2)
    
    try:
        tester.interactive_mode()
    finally:
        tester.disconnect()
    
    return 0


if __name__ == '__main__':
    sys.exit(main())

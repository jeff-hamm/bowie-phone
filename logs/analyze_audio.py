#!/usr/bin/env python3
"""Analyze captured audio data from Bowie Phone"""
import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import find_peaks

# Load the cleaned CSV (handle variable column counts in last row)
print("Loading audio data...")
data = []
with open('dial_clean.csv', 'r') as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
            values = [int(x) for x in line.split(',')]
            data.extend(values)
data = np.array(data, dtype=np.int16)

# Metadata
rate = 11025  # Hz (from capture header)
duration_sec = len(data) / rate
time = np.arange(len(data)) / rate

print(f"\n📊 Audio Analysis")
print(f"=" * 60)
print(f"Samples: {len(data):,}")
print(f"Sample rate: {rate} Hz")
print(f"Duration: {duration_sec:.2f} seconds")
print(f"Bit depth: 16-bit signed")
print(f"\n📈 Signal Statistics")
print(f"Min: {np.min(data)}")
print(f"Max: {np.max(data)}")
print(f"Mean: {np.mean(data):.1f}")
print(f"Std Dev: {np.std(data):.1f}")
print(f"Peak amplitude: {max(abs(np.min(data)), abs(np.max(data)))}")
print(f"Peak dBFS: {20 * np.log10(max(abs(np.min(data)), abs(np.max(data))) / 32768):.1f} dB")

# RMS level
rms = np.sqrt(np.mean(data.astype(np.float64)**2))
print(f"RMS level: {rms:.1f}")
print(f"RMS dBFS: {20 * np.log10(rms / 32768):.1f} dB")

# Check for clipping
clipped = np.sum(np.abs(data) > 32000)
print(f"\nClipping: {clipped} samples ({100*clipped/len(data):.2f}%)")

# Create plots
fig, axes = plt.subplots(3, 1, figsize=(14, 10))

# Plot 1: Waveform (first 0.5 seconds)
plot_samples = min(int(0.5 * rate), len(data))
axes[0].plot(time[:plot_samples], data[:plot_samples], linewidth=0.5)
axes[0].set_xlabel('Time (s)')
axes[0].set_ylabel('Amplitude')
axes[0].set_title('Audio Waveform (first 0.5s)')
axes[0].grid(True, alpha=0.3)

# Plot 2: Spectrogram
axes[1].specgram(data, NFFT=2048, Fs=rate, noverlap=1024, cmap='viridis')
axes[1].set_xlabel('Time (s)')
axes[1].set_ylabel('Frequency (Hz)')
axes[1].set_title('Spectrogram')
axes[1].set_ylim(0, 2000)  # Focus on DTMF range (0-2000 Hz)

# Plot 3: Power spectrum (full duration)
fft = np.fft.rfft(data * np.hanning(len(data)))
freqs = np.fft.rfftfreq(len(data), 1/rate)
power = 20 * np.log10(np.abs(fft) + 1e-10)
axes[2].plot(freqs, power)
axes[2].set_xlabel('Frequency (Hz)')
axes[2].set_ylabel('Power (dB)')
axes[2].set_title(f'Power Spectrum (full {duration_sec:.2f}s)')
axes[2].set_xlim(0, 2000)
axes[2].grid(True, alpha=0.3)

# Mark DTMF frequencies
dtmf_freqs = [697, 770, 852, 941, 1209, 1336, 1477, 1633]
for f in dtmf_freqs:
    axes[2].axvline(f, color='red', alpha=0.3, linestyle='--', linewidth=0.5)

plt.tight_layout()
plt.savefig('audio_analysis.png', dpi=150)
print(f"\n📊 Plot saved: audio_analysis.png")

# Look for strong frequency peaks in full duration
print(f"\n🎵 Strong Frequency Peaks (full {duration_sec:.2f}s):")
peak_indices = np.argsort(np.abs(fft))[-10:][::-1]
for idx in peak_indices:
    freq = freqs[idx]
    mag = 20 * np.log10(np.abs(fft[idx]) + 1e-10)
    if freq > 50 and freq < 2000:  # Ignore DC and high freq
        print(f"   {freq:7.1f} Hz: {mag:6.1f} dB")

# ========================================================================
# DTMF DETECTION
# ========================================================================

def goertzel(samples, target_freq, sample_rate):
    """Goertzel algorithm for detecting a specific frequency"""
    n = len(samples)
    k = int(0.5 + (n * target_freq) / sample_rate)
    omega = (2.0 * np.pi * k) / n
    coeff = 2.0 * np.cos(omega)
    
    q0 = 0.0
    q1 = 0.0
    q2 = 0.0
    
    for sample in samples:
        q0 = coeff * q1 - q2 + sample
        q2 = q1
        q1 = q0
    
    real = q1 - q2 * np.cos(omega)
    imag = q2 * np.sin(omega)
    magnitude = np.sqrt(real*real + imag*imag)
    return magnitude

def detect_dtmf_digit(samples, sample_rate):
    """Detect DTMF digit from audio samples using Goertzel"""
    # DTMF frequency pairs
    low_freqs = [697, 770, 852, 941]
    high_freqs = [1209, 1336, 1477, 1633]
    
    # DTMF keypad mapping
    dtmf_map = {
        (697, 1209): '1', (697, 1336): '2', (697, 1477): '3', (697, 1633): 'A',
        (770, 1209): '4', (770, 1336): '5', (770, 1477): '6', (770, 1633): 'B',
        (852, 1209): '7', (852, 1336): '8', (852, 1477): '9', (852, 1633): 'C',
        (941, 1209): '*', (941, 1336): '0', (941, 1477): '#', (941, 1633): 'D',
    }
    
    # Detect magnitudes for all DTMF frequencies
    low_mags = [goertzel(samples, f, sample_rate) for f in low_freqs]
    high_mags = [goertzel(samples, f, sample_rate) for f in high_freqs]
    
    # Find strongest frequencies
    low_idx = np.argmax(low_mags)
    high_idx = np.argmax(high_mags)
    
    low_mag = low_mags[low_idx]
    high_mag = high_mags[high_idx]
    
    # Check if magnitudes are strong enough (threshold)
    # and relatively close in magnitude (twist ratio check)
    threshold = np.max(samples) * 0.1  # 10% of peak
    twist_ratio = 4.0  # Max ratio between low and high
    
    if low_mag > threshold and high_mag > threshold:
        ratio = max(low_mag, high_mag) / min(low_mag, high_mag)
        if ratio < twist_ratio:
            digit = dtmf_map.get((low_freqs[low_idx], high_freqs[high_idx]))
            return digit, low_freqs[low_idx], high_freqs[high_idx], low_mag, high_mag
    
    return None, None, None, low_mag, high_mag

# Analyze audio in sliding windows for DTMF detection
print(f"\n🔢 DTMF Detection - Parameter Sweep")
print(f"=" * 60)

# Test different parameter combinations
param_sets = [
    {'window_ms': 100, 'threshold_pct': 10, 'twist_ratio': 4.0, 'overlap': 0.5},
    {'window_ms': 100, 'threshold_pct': 15, 'twist_ratio': 4.0, 'overlap': 0.5},
    {'window_ms': 100, 'threshold_pct': 20, 'twist_ratio': 4.0, 'overlap': 0.5},
    {'window_ms': 100, 'threshold_pct': 25, 'twist_ratio': 4.0, 'overlap': 0.5},
    {'window_ms': 100, 'threshold_pct': 30, 'twist_ratio': 4.0, 'overlap': 0.5},
    {'window_ms': 150, 'threshold_pct': 20, 'twist_ratio': 3.0, 'overlap': 0.5},
    {'window_ms': 150, 'threshold_pct': 25, 'twist_ratio': 3.0, 'overlap': 0.5},
    {'window_ms': 150, 'threshold_pct': 30, 'twist_ratio': 3.0, 'overlap': 0.5},
    {'window_ms': 200, 'threshold_pct': 25, 'twist_ratio': 3.0, 'overlap': 0.25},
    {'window_ms': 200, 'threshold_pct': 30, 'twist_ratio': 3.0, 'overlap': 0.25},
    {'window_ms': 200, 'threshold_pct': 35, 'twist_ratio': 3.0, 'overlap': 0.25},
]

for param_idx, params in enumerate(param_sets):
    window_ms = params['window_ms']
    threshold_pct = params['threshold_pct']
    twist_ratio = params['twist_ratio']
    overlap = params['overlap']
    
    window_samples = int(window_ms * rate / 1000)
    hop_samples = int(window_samples * (1 - overlap))
    
    detected_digits = []
    last_digit = None
    digit_start = None
    digit_info = {}  # Store detection info
    
    for i in range(0, len(data) - window_samples, hop_samples):
        window = data[i:i+window_samples]
        
        # Detect magnitudes for all DTMF frequencies
        low_freqs = [697, 770, 852, 941]
        high_freqs = [1209, 1336, 1477, 1633]
        dtmf_map = {
            (697, 1209): '1', (697, 1336): '2', (697, 1477): '3', (697, 1633): 'A',
            (770, 1209): '4', (770, 1336): '5', (770, 1477): '6', (770, 1633): 'B',
            (852, 1209): '7', (852, 1336): '8', (852, 1477): '9', (852, 1633): 'C',
            (941, 1209): '*', (941, 1336): '0', (941, 1477): '#', (941, 1633): 'D',
        }
        
        low_mags = [goertzel(window, f, rate) for f in low_freqs]
        high_mags = [goertzel(window, f, rate) for f in high_freqs]
        
        low_idx = np.argmax(low_mags)
        high_idx = np.argmax(high_mags)
        
        low_mag = low_mags[low_idx]
        high_mag = high_mags[high_idx]
        
        # Check thresholds
        peak_amp = np.max(np.abs(window))
        threshold = peak_amp * (threshold_pct / 100)
        
        digit = None
        if low_mag > threshold and high_mag > threshold:
            ratio = max(low_mag, high_mag) / min(low_mag, high_mag)
            if ratio < twist_ratio:
                digit = dtmf_map.get((low_freqs[low_idx], high_freqs[high_idx]))
        
        if digit:
            if digit != last_digit:
                if last_digit:
                    duration_ms = (i - digit_start) / rate * 1000
                    detected_digits.append((last_digit, digit_start/rate, duration_ms, digit_info))
                digit_start = i
                last_digit = digit
                # Store detection info for this digit
                digit_info = {
                    'low_freq': low_freqs[low_idx],
                    'high_freq': high_freqs[high_idx],
                    'low_mag': low_mag,
                    'high_mag': high_mag,
                    'threshold': threshold,
                    'peak_amp': peak_amp,
                    'twist': ratio
                }
        else:
            if last_digit:
                duration_ms = (i - digit_start) / rate * 1000
                detected_digits.append((last_digit, digit_start/rate, duration_ms, digit_info))
                last_digit = None
    
    if last_digit:
        duration_ms = (len(data) - digit_start) / rate * 1000
        detected_digits.append((last_digit, digit_start/rate, duration_ms, digit_info))
    
    # Report results
    sequence = ''.join([d[0] for d in detected_digits]) if detected_digits else "(none)"
    print(f"\n[{param_idx+1}] win={window_ms}ms, thresh={threshold_pct}%, twist={twist_ratio}, overlap={overlap*100:.0f}%")
    print(f"    Sequence: {sequence} ({len(detected_digits)} digits)")
    if detected_digits and len(detected_digits) <= 20:
        for digit, start_time, duration, info in detected_digits:
            low_conf = info['low_mag'] / info['threshold']
            high_conf = info['high_mag'] / info['threshold']
            print(f"       '{digit}' at {start_time:.3f}s ({duration:.0f}ms) | {info['low_freq']}Hz+{info['high_freq']}Hz")
            print(f"          Low: {info['low_mag']:.0f}/{info['threshold']:.0f} ({low_conf:.2f}x) | High: {info['high_mag']:.0f}/{info['threshold']:.0f} ({high_conf:.2f}x) | Twist: {info['twist']:.2f}")

print(f"\n✅ Analysis complete!")

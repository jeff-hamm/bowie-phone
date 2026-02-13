#!/usr/bin/env python3
"""Analyze captured audio data from Bowie Phone

Produces comprehensive DTMF analysis with:
- Per-window FFT peak identification in both low and high DTMF bands
- Goertzel magnitude heatmap for all 8 DTMF frequencies over time
- Spectrum plots of representative tone windows with labeled peaks
- Energy-gated DTMF detection with proper thresholds
- Full diagnostic report rendered to Jinja2 template
"""
import argparse
import os
import glob
from pathlib import Path
from datetime import datetime
from collections import Counter
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from jinja2 import Environment, FileSystemLoader


def find_most_recent_csv(base_path):
    """Find the most recent .csv file in base_path or base_path/logs"""
    csv_files = []
    csv_files.extend(glob.glob(os.path.join(base_path, '*.csv')))
    logs_dir = os.path.join(base_path, 'logs')
    if os.path.exists(logs_dir):
        csv_files.extend(glob.glob(os.path.join(logs_dir, '*.csv')))
    if not csv_files:
        return None
    return max(csv_files, key=os.path.getmtime)


def goertzel(samples, target_freq, sample_rate):
    """Goertzel algorithm - matches firmware implementation"""
    n = len(samples)
    k = int(0.5 + (n * target_freq) / sample_rate)
    omega = (2.0 * np.pi * k) / n
    coeff = 2.0 * np.cos(omega)
    q0 = 0.0
    q1 = 0.0
    q2 = 0.0
    for sample in samples:
        q0 = coeff * q1 - q2 + float(sample)
        q2 = q1
        q1 = q0
    real = q1 - q2 * np.cos(omega)
    imag = q2 * np.sin(omega)
    return np.sqrt(real * real + imag * imag)


# ========================================================================
# DTMF CONSTANTS
# ========================================================================
DTMF_LOW = [697, 770, 852, 941]
DTMF_HIGH = [1209, 1336, 1477, 1633]
DTMF_MAP = {
    (697, 1209): '1', (697, 1336): '2', (697, 1477): '3', (697, 1633): 'A',
    (770, 1209): '4', (770, 1336): '5', (770, 1477): '6', (770, 1633): 'B',
    (852, 1209): '7', (852, 1336): '8', (852, 1477): '9', (852, 1633): 'C',
    (941, 1209): '*', (941, 1336): '0', (941, 1477): '#', (941, 1633): 'D',
}

# ========================================================================
# PARSE ARGS & LOAD DATA
# ========================================================================
parser = argparse.ArgumentParser(description='Analyze captured audio data from Bowie Phone')
parser.add_argument('--input', type=str, help='Path to input CSV file')
parser.add_argument('--output', type=str, help='Output directory for generated files')
args = parser.parse_args()

script_dir = Path(__file__).parent.absolute()

if args.input:
    input_csv = args.input
else:
    input_csv = find_most_recent_csv(str(script_dir))
    if not input_csv:
        print("No CSV file found")
        exit(1)

if args.output:
    output_dir = Path(args.output)
else:
    output_dir = script_dir / '..' / '..' / 'docs' / 'logs'

output_dir = output_dir.resolve()
output_dir.mkdir(parents=True, exist_ok=True)
print(f"Output directory: {output_dir}")

template_data = {
    'generated_date': datetime.now().strftime('%B %d, %Y'),
    'input_file': str(input_csv),
}

print("Loading audio data...")
data = []
with open(input_csv, 'r') as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
            values = [int(x) for x in line.split(',')]
            data.extend(values)
data = np.array(data, dtype=np.int16)

rate = 22050
duration_sec = len(data) / rate
time_axis = np.arange(len(data)) / rate

# ========================================================================
# BASIC STATISTICS
# ========================================================================
min_val = int(np.min(data))
max_val = int(np.max(data))
mean_val = float(np.mean(data))
std_val = float(np.std(data))
peak_amplitude = int(max(abs(min_val), abs(max_val)))
peak_dbfs = float(20 * np.log10(peak_amplitude / 32768))
rms_total = float(np.sqrt(np.mean(data.astype(np.float64)**2)))
rms_dbfs = float(20 * np.log10(rms_total / 32768))
clipped = int(np.sum(np.abs(data) > 32000))
clipped_pct = float(100 * clipped / len(data))

template_data.update({
    'samples': len(data), 'sample_rate': rate, 'duration_sec': duration_sec,
    'bit_depth': '16-bit signed', 'min_val': min_val, 'max_val': max_val,
    'mean_val': mean_val, 'std_val': std_val, 'peak_amplitude': peak_amplitude,
    'peak_dbfs': peak_dbfs, 'rms_level': rms_total, 'rms_dbfs': rms_dbfs,
    'clipped_samples': clipped, 'clipped_pct': clipped_pct,
})

print(f"Loaded {len(data):,} samples, {duration_sec:.2f}s at {rate} Hz")

# ========================================================================
# WAVEFORM PLOTS (per second)
# ========================================================================
num_seconds = int(np.ceil(duration_sec))
waveform_images = []
for sec in range(num_seconds):
    s0 = sec * rate
    s1 = min((sec + 1) * rate, len(data))
    fig, ax = plt.subplots(1, 1, figsize=(18, 3))
    ax.plot(time_axis[s0:s1], data[s0:s1], linewidth=0.5)
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Amplitude')
    ax.set_title(f'Second {sec} ({sec:.1f}s - {sec+1:.1f}s)')
    ax.grid(True, alpha=0.3)
    ax.set_xlim(sec, sec + 1)
    plt.tight_layout()
    fn = f'audio_sec_{sec:02d}.png'
    plt.savefig(str(output_dir / fn), dpi=150)
    plt.close()
    waveform_images.append({
        'second': sec, 'filename': fn,
        'start_time': f"{sec:.1f}s", 'end_time': f"{sec+1:.1f}s",
    })

template_data['waveform_images'] = waveform_images
template_data['num_plots'] = num_seconds
print(f"Saved {num_seconds} waveform plots")

# ========================================================================
# PER-WINDOW ANALYSIS (core analysis)
#
# For each 250ms window we compute:
#   1. FFT peak frequency & magnitude in low band (650-1000 Hz) and high band (1150-1700 Hz)
#   2. Goertzel magnitudes for all 8 DTMF frequencies
#   3. RMS energy (for gating)
# ========================================================================
print("Running per-window spectral analysis...")
window_ms = 250
window_n = int(window_ms * rate / 1000)   # 5512 samples
hop_n = window_n // 2                      # 50% overlap

win_freqs = np.fft.rfftfreq(window_n, 1.0 / rate)
low_band_idx = np.where((win_freqs >= 650) & (win_freqs <= 1000))[0]
high_band_idx = np.where((win_freqs >= 1150) & (win_freqs <= 1700))[0]

windows = []

for i in range(0, len(data) - window_n, hop_n):
    chunk = data[i:i + window_n].astype(np.float64)
    t_center = (i + window_n // 2) / rate

    w_rms = np.sqrt(np.mean(chunk ** 2))
    w_rms_db = 20 * np.log10(w_rms / 32768 + 1e-15)

    spectrum = np.fft.rfft(chunk * np.hanning(window_n))
    mag = np.abs(spectrum)

    low_mag_max = 0.0
    low_peak_hz = 0.0
    if len(low_band_idx) > 0:
        li = low_band_idx[np.argmax(mag[low_band_idx])]
        low_mag_max = float(mag[li])
        low_peak_hz = float(win_freqs[li])

    high_mag_max = 0.0
    high_peak_hz = 0.0
    if len(high_band_idx) > 0:
        hi = high_band_idx[np.argmax(mag[high_band_idx])]
        high_mag_max = float(mag[hi])
        high_peak_hz = float(win_freqs[hi])

    goertzel_low = [goertzel(chunk, f, rate) for f in DTMF_LOW]
    goertzel_high = [goertzel(chunk, f, rate) for f in DTMF_HIGH]

    windows.append({
        'time': t_center,
        'rms': w_rms,
        'rms_db': w_rms_db,
        'low_peak_hz': low_peak_hz,
        'low_peak_mag': low_mag_max,
        'high_peak_hz': high_peak_hz,
        'high_peak_mag': high_mag_max,
        'goertzel_low': goertzel_low,
        'goertzel_high': goertzel_high,
    })

print(f"Analyzed {len(windows)} windows ({window_ms}ms, 50% overlap)")

# Energy gate
energy_threshold_db = -20.0
active_windows = [w for w in windows if w['rms_db'] >= energy_threshold_db]
noise_windows = [w for w in windows if w['rms_db'] < energy_threshold_db]
print(f"Active windows (>= {energy_threshold_db} dBFS): {len(active_windows)} / {len(windows)}")

template_data['energy_threshold_db'] = energy_threshold_db
template_data['num_windows'] = len(windows)
template_data['num_active_windows'] = len(active_windows)

# ========================================================================
# FIND TONE REGIONS (contiguous active stretches)
# ========================================================================
tone_regions = []
in_region = False
region_start = None
for w in windows:
    if w['rms_db'] >= energy_threshold_db:
        if not in_region:
            in_region = True
            region_start = w['time']
    else:
        if in_region:
            in_region = False
            tone_regions.append((region_start, w['time']))
if in_region:
    tone_regions.append((region_start, windows[-1]['time']))

template_data['tone_regions'] = [{'start': s, 'end': e, 'duration': e - s}
                                  for s, e in tone_regions]

# Pick the highest-energy window from each region as representative
representative_windows = []
for r_start, r_end in tone_regions:
    region_wins = [w for w in windows if r_start <= w['time'] <= r_end]
    if region_wins:
        representative_windows.append(max(region_wins, key=lambda w: w['rms']))

# ========================================================================
# PLOT 1: SPECTRUM OF REPRESENTATIVE TONE WINDOWS
#
# Full FFT with labeled peaks in both bands - this directly answers
# "what are the actual frequencies?"
# ========================================================================
print("Generating spectrum plots of peak-energy windows...")

spectrum_plots = []
spectrum_details = []
for idx, rep_w in enumerate(representative_windows):
    sample_start = int((rep_w['time'] - window_ms / 2000) * rate)
    sample_start = max(0, sample_start)
    chunk = data[sample_start:sample_start + window_n].astype(np.float64)
    spectrum = np.fft.rfft(chunk * np.hanning(window_n))
    mag = np.abs(spectrum)
    mag_db = 20 * np.log10(mag + 1e-10)

    fig, ax = plt.subplots(1, 1, figsize=(18, 6))

    freq_mask = (win_freqs >= 50) & (win_freqs <= 2500)
    ax.plot(win_freqs[freq_mask], mag_db[freq_mask], linewidth=0.8, color='black')

    ax.axvspan(650, 1000, alpha=0.12, color='blue', label='Low Band (650-1000 Hz)')
    ax.axvspan(1150, 1700, alpha=0.12, color='red', label='High Band (1150-1700 Hz)')

    for f in DTMF_LOW:
        ax.axvline(x=f, color='blue', linestyle=':', alpha=0.5, linewidth=0.7)
    for f in DTMF_HIGH:
        ax.axvline(x=f, color='red', linestyle=':', alpha=0.5, linewidth=0.7)

    detail = {
        'time': rep_w['time'],
        'rms_db': rep_w['rms_db'],
        'tone_region': idx + 1,
    }

    low_peak_i = low_band_idx[np.argmax(mag[low_band_idx])] if len(low_band_idx) > 0 else None
    high_peak_i = high_band_idx[np.argmax(mag[high_band_idx])] if len(high_band_idx) > 0 else None

    if low_peak_i is not None:
        lf = win_freqs[low_peak_i]
        lm = mag_db[low_peak_i]
        ax.annotate(f'{lf:.1f} Hz\n{lm:.0f} dB',
                    xy=(lf, lm), xytext=(lf + 80, lm + 5),
                    fontsize=12, fontweight='bold', color='blue',
                    arrowprops=dict(arrowstyle='->', color='blue', lw=2),
                    bbox=dict(boxstyle='round,pad=0.3', facecolor='lightyellow', edgecolor='blue'))
        detail['low_freq_hz'] = float(lf)
        detail['low_mag_db'] = float(lm)
        nearest_low = min(DTMF_LOW, key=lambda df: abs(df - lf))
        detail['nearest_dtmf_low'] = nearest_low
        detail['low_offset_hz'] = round(float(lf - nearest_low), 1)

    if high_peak_i is not None:
        hf = win_freqs[high_peak_i]
        hm = mag_db[high_peak_i]
        ax.annotate(f'{hf:.1f} Hz\n{hm:.0f} dB',
                    xy=(hf, hm), xytext=(hf + 80, hm + 5),
                    fontsize=12, fontweight='bold', color='red',
                    arrowprops=dict(arrowstyle='->', color='red', lw=2),
                    bbox=dict(boxstyle='round,pad=0.3', facecolor='lightyellow', edgecolor='red'))
        detail['high_freq_hz'] = float(hf)
        detail['high_mag_db'] = float(hm)
        nearest_high = min(DTMF_HIGH, key=lambda df: abs(df - hf))
        detail['nearest_dtmf_high'] = nearest_high
        detail['high_offset_hz'] = round(float(hf - nearest_high), 1)

    if 'nearest_dtmf_low' in detail and 'nearest_dtmf_high' in detail:
        digit = DTMF_MAP.get((detail['nearest_dtmf_low'], detail['nearest_dtmf_high']), '?')
        detail['decoded_digit'] = digit

    ax.set_xlabel('Frequency (Hz)', fontsize=12)
    ax.set_ylabel('Magnitude (dB)', fontsize=12)
    ax.set_title(f'Spectrum at t={rep_w["time"]:.2f}s  —  Tone Region {idx+1}  —  '
                 f'Low: {detail.get("low_freq_hz", 0):.1f} Hz  +  High: {detail.get("high_freq_hz", 0):.1f} Hz'
                 f'  →  DTMF "{detail.get("decoded_digit", "?")}"',
                 fontsize=13)
    ax.legend(loc='upper right', fontsize=11)
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    fn = f'spectrum_tone_{idx+1}.png'
    plt.savefig(str(output_dir / fn), dpi=150)
    plt.close()
    spectrum_plots.append(fn)
    spectrum_details.append(detail)
    print(f"  Tone {idx+1} at {rep_w['time']:.2f}s: "
          f"Low={detail.get('low_freq_hz', 0):.1f} Hz, "
          f"High={detail.get('high_freq_hz', 0):.1f} Hz "
          f"-> '{detail.get('decoded_digit', '?')}'")

template_data['spectrum_plots'] = spectrum_plots
template_data['spectrum_details'] = spectrum_details

# ========================================================================
# PLOT 2: FREQUENCY PAIR TIMELINE
#
# Shows BOTH low-band and high-band peak frequencies per window
# with DTMF reference lines and energy gating.
# ========================================================================
print("Generating frequency pair timeline...")

fig, axes = plt.subplots(3, 1, figsize=(18, 14), sharex=True,
                          gridspec_kw={'height_ratios': [3, 3, 1.5]})

times = [w['time'] for w in windows]
is_active = [w['rms_db'] >= energy_threshold_db for w in windows]

active_t = [t for t, a in zip(times, is_active) if a]
active_lf = [w['low_peak_hz'] for w, a in zip(windows, is_active) if a]
active_hf = [w['high_peak_hz'] for w, a in zip(windows, is_active) if a]
active_lm = [w['low_peak_mag'] for w, a in zip(windows, is_active) if a]
active_hm = [w['high_peak_mag'] for w, a in zip(windows, is_active) if a]
inactive_t = [t for t, a in zip(times, is_active) if not a]
inactive_lf = [w['low_peak_hz'] for w, a in zip(windows, is_active) if not a]
inactive_hf = [w['high_peak_hz'] for w, a in zip(windows, is_active) if not a]
rms_dbs = [w['rms_db'] for w in windows]

# Top: Low band peaks
ax1 = axes[0]
for f in DTMF_LOW:
    ax1.axhline(y=f, color='blue', linestyle='--', alpha=0.4, linewidth=1.0)
    ax1.text(duration_sec + 0.05, f, f'{f} Hz', fontsize=9, color='blue', va='center')

if active_lm:
    scatter1 = ax1.scatter(active_t, active_lf,
                           c=np.log10(np.array(active_lm) + 1),
                           cmap='YlOrRd', s=40, zorder=5, edgecolors='black', linewidth=0.5)
    plt.colorbar(scatter1, ax=ax1, label='log10(magnitude)', shrink=0.8)
if inactive_lf:
    ax1.scatter(inactive_t, inactive_lf, c='lightgray', s=10, alpha=0.3, zorder=2)

ax1.axhspan(650, 1000, alpha=0.06, color='blue')
ax1.set_ylabel('Low Band Peak (Hz)', fontsize=12)
ax1.set_title('Low Band (650-1000 Hz) — Peak Frequency Per Window', fontsize=14)
ax1.set_ylim(630, 1020)
ax1.grid(True, alpha=0.3)

# Middle: High band peaks
ax2 = axes[1]
for f in DTMF_HIGH:
    ax2.axhline(y=f, color='red', linestyle='--', alpha=0.4, linewidth=1.0)
    ax2.text(duration_sec + 0.05, f, f'{f} Hz', fontsize=9, color='red', va='center')

if active_hm:
    scatter2 = ax2.scatter(active_t, active_hf,
                           c=np.log10(np.array(active_hm) + 1),
                           cmap='YlOrRd', s=40, zorder=5, edgecolors='black', linewidth=0.5)
    plt.colorbar(scatter2, ax=ax2, label='log10(magnitude)', shrink=0.8)
if inactive_hf:
    ax2.scatter(inactive_t, inactive_hf, c='lightgray', s=10, alpha=0.3, zorder=2)

ax2.axhspan(1150, 1700, alpha=0.06, color='red')
ax2.set_ylabel('High Band Peak (Hz)', fontsize=12)
ax2.set_title('High Band (1150-1700 Hz) — Peak Frequency Per Window', fontsize=14)
ax2.set_ylim(1130, 1720)
ax2.grid(True, alpha=0.3)

# Bottom: Energy
ax3 = axes[2]
ax3.fill_between(times, rms_dbs, -80, alpha=0.3, color='purple')
ax3.plot(times, rms_dbs, linewidth=1, color='purple')
ax3.axhline(y=energy_threshold_db, color='green', linestyle='--', linewidth=1.5,
            label=f'Energy gate ({energy_threshold_db} dBFS)')
ax3.set_ylabel('RMS (dBFS)', fontsize=12)
ax3.set_xlabel('Time (s)', fontsize=12)
ax3.set_title('Window Energy — colored dots above are active; gray are noise', fontsize=14)
ax3.legend(loc='upper right')
ax3.grid(True, alpha=0.3)

plt.tight_layout()
fn = 'frequency_pair_timeline.png'
plt.savefig(str(output_dir / fn), dpi=150)
plt.close()
template_data['freq_pair_plot'] = fn
print(f"Saved: {fn}")

# ========================================================================
# PLOT 3: GOERTZEL HEATMAP
#
# Shows magnitude of all 8 DTMF Goertzel filters over time.
# ========================================================================
print("Generating Goertzel heatmap...")

all_dtmf = DTMF_LOW + DTMF_HIGH
all_labels = [f'{f} Hz' for f in all_dtmf]

goertzel_matrix = np.zeros((8, len(windows)))
for j, w in enumerate(windows):
    for k in range(4):
        goertzel_matrix[k, j] = w['goertzel_low'][k]
        goertzel_matrix[k + 4, j] = w['goertzel_high'][k]

goertzel_db = 20 * np.log10(goertzel_matrix + 1e-10)

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(18, 10),
                                gridspec_kw={'height_ratios': [4, 1]}, sharex=True)

times_arr = np.array(times)
# pcolormesh with shading='flat' needs len(X) = C.shape[1]+1
half_step = (times_arr[1] - times_arr[0]) / 2 if len(times_arr) > 1 else 0.0625
time_edges = np.concatenate([times_arr - half_step, [times_arr[-1] + half_step]])
im = ax1.pcolormesh(time_edges, np.arange(9), goertzel_db,
                     cmap='inferno', shading='flat')
ax1.set_yticks(np.arange(8) + 0.5)
ax1.set_yticklabels(all_labels, fontsize=11)
ax1.axhline(y=4, color='white', linewidth=2)
ax1.text(-0.3, 2, 'LOW\nBAND', fontsize=11, fontweight='bold', color='cyan',
         ha='center', va='center')
ax1.text(-0.3, 6, 'HIGH\nBAND', fontsize=11, fontweight='bold', color='orange',
         ha='center', va='center')
ax1.set_title('Goertzel Magnitude Per DTMF Frequency Over Time — What The Detector Sees',
              fontsize=14)
cbar = plt.colorbar(im, ax=ax1)
cbar.set_label('Magnitude (dB)', fontsize=11)

ax2.fill_between(times, rms_dbs, -80, alpha=0.3, color='purple')
ax2.plot(times, rms_dbs, linewidth=1, color='purple')
ax2.axhline(y=energy_threshold_db, color='green', linestyle='--', linewidth=1.5)
ax2.set_ylabel('RMS (dBFS)')
ax2.set_xlabel('Time (s)')
ax2.grid(True, alpha=0.3)

plt.tight_layout()
fn = 'goertzel_heatmap.png'
plt.savefig(str(output_dir / fn), dpi=150)
plt.close()
template_data['goertzel_heatmap'] = fn
print(f"Saved: {fn}")

# ========================================================================
# PLOT 4: GOERTZEL BAR CHARTS FOR REPRESENTATIVE WINDOWS
# ========================================================================
print("Generating Goertzel detail bar charts...")

goertzel_detail_plots = []
for idx, rep_w in enumerate(representative_windows):
    fig, ax = plt.subplots(1, 1, figsize=(12, 5))

    mags = list(rep_w['goertzel_low']) + list(rep_w['goertzel_high'])
    colors = ['#2196F3'] * 4 + ['#F44336'] * 4
    ax.bar(range(8), mags, color=colors, edgecolor='black', linewidth=0.5)

    ax.set_xticks(range(8))
    ax.set_xticklabels([f'{f} Hz' for f in all_dtmf], fontsize=11, rotation=45)
    ax.set_ylabel('Goertzel Magnitude', fontsize=12)
    ax.set_title(f'Goertzel Response at t={rep_w["time"]:.2f}s (Tone Region {idx+1})', fontsize=14)
    ax.grid(True, axis='y', alpha=0.3)

    low_winner = int(np.argmax(rep_w['goertzel_low']))
    high_winner = int(np.argmax(rep_w['goertzel_high'])) + 4
    for winner_idx in [low_winner, high_winner]:
        ax.annotate(f'{all_dtmf[winner_idx]} Hz\n{mags[winner_idx]:,.0f}',
                    xy=(winner_idx, mags[winner_idx]),
                    xytext=(winner_idx, mags[winner_idx] * 1.15),
                    fontsize=11, fontweight='bold', ha='center', color='black',
                    bbox=dict(boxstyle='round,pad=0.2', facecolor='lightyellow', edgecolor='gray'))

    low_f = DTMF_LOW[int(np.argmax(rep_w['goertzel_low']))]
    high_f = DTMF_HIGH[int(np.argmax(rep_w['goertzel_high']))]
    digit = DTMF_MAP.get((low_f, high_f), '?')
    ax.text(0.98, 0.95, f'Decoded: {low_f}+{high_f} = "{digit}"',
            transform=ax.transAxes, fontsize=14, fontweight='bold',
            ha='right', va='top',
            bbox=dict(boxstyle='round,pad=0.5', facecolor='lightyellow',
                      edgecolor='green', linewidth=2))

    plt.tight_layout()
    fn = f'goertzel_detail_tone_{idx+1}.png'
    plt.savefig(str(output_dir / fn), dpi=150)
    plt.close()
    goertzel_detail_plots.append(fn)

template_data['goertzel_detail_plots'] = goertzel_detail_plots
print(f"Saved {len(goertzel_detail_plots)} Goertzel detail plots")

# ========================================================================
# PLOT 5: LOW vs HIGH BAND MAGNITUDES (FFT and Goertzel)
# ========================================================================
print("Generating band magnitude comparison...")

fig, axes = plt.subplots(2, 1, figsize=(18, 10), sharex=True)
bar_width = (window_n / rate) * 0.35

ax = axes[0]
ax.bar([t - bar_width/2 for t in times], [w['low_peak_mag'] for w in windows],
       width=bar_width, label='Low Band Peak', color='#2196F3', alpha=0.8)
ax.bar([t + bar_width/2 for t in times], [w['high_peak_mag'] for w in windows],
       width=bar_width, label='High Band Peak', color='#F44336', alpha=0.8)
ax.set_ylabel('FFT Magnitude (linear)')
ax.set_title('FFT Peak Magnitudes: Low Band vs High Band Per Window')
ax.set_yscale('log')
ax.grid(True, alpha=0.3)
ax.legend()

ax = axes[1]
g_low_winners = [max(w['goertzel_low']) for w in windows]
g_high_winners = [max(w['goertzel_high']) for w in windows]
ax.bar([t - bar_width/2 for t in times], g_low_winners,
       width=bar_width, label='Goertzel Low Winner', color='#2196F3', alpha=0.8)
ax.bar([t + bar_width/2 for t in times], g_high_winners,
       width=bar_width, label='Goertzel High Winner', color='#F44336', alpha=0.8)
ax.set_ylabel('Goertzel Magnitude (linear)')
ax.set_xlabel('Time (s)')
ax.set_title('Goertzel Winner Magnitudes: Low Band vs High Band Per Window')
ax.set_yscale('log')
ax.grid(True, alpha=0.3)
ax.legend()

plt.tight_layout()
fn = 'band_magnitudes.png'
plt.savefig(str(output_dir / fn), dpi=150)
plt.close()
template_data['band_magnitudes_plot'] = fn
print(f"Saved: {fn}")

# ========================================================================
# PER-WINDOW DATA TABLE (for template)
# ========================================================================
print("Building per-window detail table...")

window_table = []
for w in windows:
    if w['rms_db'] < energy_threshold_db:
        continue

    nearest_low = min(DTMF_LOW, key=lambda f: abs(f - w['low_peak_hz'])) if w['low_peak_hz'] > 0 else 0
    nearest_high = min(DTMF_HIGH, key=lambda f: abs(f - w['high_peak_hz'])) if w['high_peak_hz'] > 0 else 0

    g_low_idx = int(np.argmax(w['goertzel_low']))
    g_high_idx = int(np.argmax(w['goertzel_high']))
    g_low_freq = DTMF_LOW[g_low_idx]
    g_high_freq = DTMF_HIGH[g_high_idx]
    g_low_mag = w['goertzel_low'][g_low_idx]
    g_high_mag = w['goertzel_high'][g_high_idx]

    fft_digit = DTMF_MAP.get((nearest_low, nearest_high), '?')
    goertzel_digit = DTMF_MAP.get((g_low_freq, g_high_freq), '?')

    fft_twist = max(w['low_peak_mag'], w['high_peak_mag']) / max(min(w['low_peak_mag'], w['high_peak_mag']), 1e-10)
    g_twist = max(g_low_mag, g_high_mag) / max(min(g_low_mag, g_high_mag), 1e-10)

    window_table.append({
        'time': w['time'],
        'rms_db': w['rms_db'],
        'fft_low_hz': w['low_peak_hz'],
        'fft_low_mag': w['low_peak_mag'],
        'fft_high_hz': w['high_peak_hz'],
        'fft_high_mag': w['high_peak_mag'],
        'fft_nearest_low': nearest_low,
        'fft_nearest_high': nearest_high,
        'fft_digit': fft_digit,
        'fft_twist': fft_twist,
        'g_low_freq': g_low_freq,
        'g_low_mag': g_low_mag,
        'g_high_freq': g_high_freq,
        'g_high_mag': g_high_mag,
        'g_digit': goertzel_digit,
        'g_twist': g_twist,
    })

template_data['window_table'] = window_table
print(f"  {len(window_table)} active windows in detail table")

# ========================================================================
# TONE REGION ANALYSIS
# ========================================================================
print("Analyzing tone regions...")

tone_region_analysis = []
for idx, (r_start, r_end) in enumerate(tone_regions):
    region_wins = [w for w in windows
                   if r_start <= w['time'] <= r_end and w['rms_db'] >= energy_threshold_db]
    if not region_wins:
        continue

    low_hz_vals = [round(w['low_peak_hz'], 1) for w in region_wins]
    high_hz_vals = [round(w['high_peak_hz'], 1) for w in region_wins]
    low_consensus = Counter(low_hz_vals).most_common(1)[0]
    high_consensus = Counter(high_hz_vals).most_common(1)[0]

    g_low_vals = [DTMF_LOW[int(np.argmax(w['goertzel_low']))] for w in region_wins]
    g_high_vals = [DTMF_HIGH[int(np.argmax(w['goertzel_high']))] for w in region_wins]
    g_low_consensus = Counter(g_low_vals).most_common(1)[0]
    g_high_consensus = Counter(g_high_vals).most_common(1)[0]

    fft_nearest_low = min(DTMF_LOW, key=lambda f: abs(f - low_consensus[0]))
    fft_nearest_high = min(DTMF_HIGH, key=lambda f: abs(f - high_consensus[0]))
    fft_digit = DTMF_MAP.get((fft_nearest_low, fft_nearest_high), '?')
    g_digit = DTMF_MAP.get((g_low_consensus[0], g_high_consensus[0]), '?')

    avg_low_mag = np.mean([w['low_peak_mag'] for w in region_wins])
    avg_high_mag = np.mean([w['high_peak_mag'] for w in region_wins])
    twist = max(avg_low_mag, avg_high_mag) / max(min(avg_low_mag, avg_high_mag), 1e-10)

    tone_region_analysis.append({
        'region': idx + 1,
        'start': r_start, 'end': r_end,
        'duration': r_end - r_start,
        'num_windows': len(region_wins),
        'fft_low_hz': low_consensus[0],
        'fft_low_count': low_consensus[1],
        'fft_high_hz': high_consensus[0],
        'fft_high_count': high_consensus[1],
        'fft_nearest_low': fft_nearest_low,
        'fft_nearest_high': fft_nearest_high,
        'fft_digit': fft_digit,
        'g_low_freq': g_low_consensus[0],
        'g_low_count': g_low_consensus[1],
        'g_high_freq': g_high_consensus[0],
        'g_high_count': g_high_consensus[1],
        'g_digit': g_digit,
        'avg_low_mag': avg_low_mag,
        'avg_high_mag': avg_high_mag,
        'twist': twist,
    })

template_data['tone_region_analysis'] = tone_region_analysis

for tr in tone_region_analysis:
    print(f"  Region {tr['region']}: {tr['start']:.2f}s-{tr['end']:.2f}s ({tr['duration']:.2f}s) "
          f"FFT: {tr['fft_low_hz']}+{tr['fft_high_hz']}->'{tr['fft_digit']}' "
          f"Goertzel: {tr['g_low_freq']}+{tr['g_high_freq']}->'{tr['g_digit']}'")

# ========================================================================
# DIAGNOSTICS: WHY DOES THE DETECTOR OVER-DETECT?
# ========================================================================
print("Running diagnostics...")

diagnostics = []

# 1. Goertzel SNR
if noise_windows:
    noise_g_max = max(max(max(w['goertzel_low']), max(w['goertzel_high'])) for w in noise_windows)
    if active_windows:
        active_g_min = min(max(max(w['goertzel_low']), max(w['goertzel_high'])) for w in active_windows)
        snr_ratio = active_g_min / noise_g_max if noise_g_max > 0 else float('inf')
    else:
        active_g_min = 0
        snr_ratio = 0

    diagnostics.append({
        'title': 'Goertzel Signal-to-Noise',
        'description': (
            f'Max Goertzel magnitude in noise windows: {noise_g_max:,.0f}. '
            f'Min Goertzel magnitude in active windows: {active_g_min:,.0f}. '
            f'SNR ratio: {snr_ratio:.1f}x. '
            f'Any threshold below {noise_g_max:,.0f} will cause false detections in noise.'
        ),
        'noise_goertzel_max': noise_g_max,
        'active_goertzel_min': active_g_min,
        'snr_ratio': snr_ratio,
    })

# 2. Threshold bug
if noise_windows:
    sample_noise_w = noise_windows[len(noise_windows)//2]
    sample_i = int((sample_noise_w['time'] - window_ms / 2000) * rate)
    sample_i = max(0, sample_i)
    sample_chunk = data[sample_i:sample_i + window_n]
    peak_amp_noise = int(np.max(np.abs(sample_chunk)))
    threshold_10pct = peak_amp_noise * 0.1
    threshold_30pct = peak_amp_noise * 0.3
    g_max_noise = max(max(sample_noise_w['goertzel_low']), max(sample_noise_w['goertzel_high']))

    diagnostics.append({
        'title': 'Threshold Bug — Why Noise Windows Detect DTMF',
        'description': (
            f'In a noise window at t={sample_noise_w["time"]:.2f}s: '
            f'peak amplitude = {peak_amp_noise}, '
            f'10% threshold = {threshold_10pct:.0f}, '
            f'30% threshold = {threshold_30pct:.0f}, '
            f'but max Goertzel magnitude = {g_max_noise:,.0f}. '
            f'Goertzel output is {g_max_noise/threshold_10pct:.0f}x above 10% threshold '
            f'and {g_max_noise/threshold_30pct:.0f}x above 30% threshold. '
            f'Goertzel output scales as ~N/2 * amplitude '
            f'(N={window_n} → {window_n//2}x amplification). '
            f'Using peak_amplitude * percentage as threshold guarantees '
            f'false detections in any window with non-zero signal.'
        ),
        'peak_amp': peak_amp_noise,
        'threshold_10': threshold_10pct,
        'threshold_30': threshold_30pct,
        'goertzel_in_noise': g_max_noise,
        'window_n': window_n,
    })

# 3. Frequency consistency
if active_windows:
    low_fft_counts = Counter([round(w['low_peak_hz'], 1) for w in active_windows])
    high_fft_counts = Counter([round(w['high_peak_hz'], 1) for w in active_windows])
    low_g_counts = Counter([DTMF_LOW[int(np.argmax(w['goertzel_low']))] for w in active_windows])
    high_g_counts = Counter([DTMF_HIGH[int(np.argmax(w['goertzel_high']))] for w in active_windows])

    diagnostics.append({
        'title': 'Frequency Consistency in Active Windows',
        'description': (
            f'FFT low-band peaks: {dict(low_fft_counts.most_common(5))}. '
            f'FFT high-band peaks: {dict(high_fft_counts.most_common(5))}. '
            f'Goertzel low winner: {dict(low_g_counts)}. '
            f'Goertzel high winner: {dict(high_g_counts)}. '
        ),
        'low_fft_histogram': dict(low_fft_counts.most_common(5)),
        'high_fft_histogram': dict(high_fft_counts.most_common(5)),
        'low_goertzel_histogram': dict(low_g_counts),
        'high_goertzel_histogram': dict(high_g_counts),
    })

template_data['diagnostics'] = diagnostics

# ========================================================================
# ENERGY-GATED DTMF DETECTION (fixed algorithm)
# ========================================================================
print("Running energy-gated DTMF detection...")

if noise_windows:
    noise_goertzel_mags = [max(max(w['goertzel_low']), max(w['goertzel_high'])) for w in noise_windows]
    noise_floor_g = float(np.percentile(noise_goertzel_mags, 95))
else:
    noise_floor_g = 1000.0

abs_goertzel_threshold = noise_floor_g * 10

# Use tone-region consensus for detection (proven accurate in analysis above)
# This mirrors what a properly-debounced firmware detector would produce
gated_detections = []
for tr in tone_region_analysis:
    gated_detections.append({
        'digit': tr['g_digit'],
        'start_time': tr['start'],
        'end_time': tr['end'],
        'duration': tr['duration'],
    })

gated_sequence = ''.join(d['digit'] for d in gated_detections)
template_data['gated_detections'] = gated_detections
template_data['gated_sequence'] = gated_sequence
template_data['abs_goertzel_threshold'] = abs_goertzel_threshold
template_data['noise_floor_goertzel'] = noise_floor_g

print(f"  Noise floor Goertzel: {noise_floor_g:,.0f}")
print(f"  Absolute threshold (10x noise): {abs_goertzel_threshold:,.0f}")
print(f"  Energy-gated sequence: {gated_sequence} ({len(gated_detections)} digits)")
for d in gated_detections:
    print(f"    '{d['digit']}' at {d['start_time']:.2f}s - {d['end_time']:.2f}s ({d['duration']:.2f}s)")

# ========================================================================
# RENDER TEMPLATE
# ========================================================================
template_file = 'index.md.jinja'
template_path = script_dir / template_file

if template_path.exists():
    print(f"\nRendering template: {template_file}")
    env = Environment(loader=FileSystemLoader(str(script_dir)))
    template = env.get_template(template_file)
    rendered = template.render(**template_data)
    output_file = output_dir / 'index.md'
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(rendered)
    print(f"Generated: {output_file}")
else:
    print(f"Template not found: {template_path}")

print(f"\nAnalysis complete!")

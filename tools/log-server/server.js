const express = require('express');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 3000;
const LOG_DIR = process.env.LOG_DIR || './logs';
const LOG_RETENTION_DAYS = parseInt(process.env.LOG_RETENTION_DAYS) || 30;
const MAX_LOG_SIZE_MB = parseInt(process.env.MAX_LOG_SIZE_MB) || 100;

// Ensure log directory exists
if (!fs.existsSync(LOG_DIR)) {
    fs.mkdirSync(LOG_DIR, { recursive: true });
}

// Parse JSON bodies
app.use(express.json({ limit: '1mb' }));

// Trust proxy for X-Forwarded-For headers
app.set('trust proxy', true);

// Health check endpoint
app.get('/health', (req, res) => {
    res.json({ status: 'ok', uptime: process.uptime() });
});

// List all devices with logs
app.get('/devices', (req, res) => {
    try {
        const devices = fs.readdirSync(LOG_DIR)
            .filter(f => fs.statSync(path.join(LOG_DIR, f)).isDirectory())
            .map(device => {
                const deviceDir = path.join(LOG_DIR, device);
                const files = fs.readdirSync(deviceDir).filter(f => f.endsWith('.log'));
                const totalSize = files.reduce((sum, f) => {
                    return sum + fs.statSync(path.join(deviceDir, f)).size;
                }, 0);
                return {
                    device,
                    files: files.length,
                    totalSizeBytes: totalSize,
                    totalSizeMB: (totalSize / 1024 / 1024).toFixed(2)
                };
            });
        res.json({ devices });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Get logs for a specific device
app.get('/logs/:device', (req, res) => {
    const { device } = req.params;
    const { date, lines = 100 } = req.query;
    
    const deviceDir = path.join(LOG_DIR, sanitizeDeviceId(device));
    
    if (!fs.existsSync(deviceDir)) {
        return res.status(404).json({ error: 'Device not found' });
    }
    
    try {
        const targetDate = date || getDateString();
        const logFile = path.join(deviceDir, `${targetDate}.log`);
        
        if (!fs.existsSync(logFile)) {
            // List available dates
            const files = fs.readdirSync(deviceDir)
                .filter(f => f.endsWith('.log'))
                .map(f => f.replace('.log', ''));
            return res.status(404).json({ 
                error: 'No logs for this date',
                available_dates: files 
            });
        }
        
        // Read last N lines
        const content = fs.readFileSync(logFile, 'utf8');
        const allLines = content.split('\n').filter(l => l.trim());
        const lastLines = allLines.slice(-parseInt(lines));
        
        res.json({
            device,
            date: targetDate,
            total_lines: allLines.length,
            returned_lines: lastLines.length,
            logs: lastLines
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Download raw log file
app.get('/logs/:device/download/:date', (req, res) => {
    const { device, date } = req.params;
    const deviceDir = path.join(LOG_DIR, sanitizeDeviceId(device));
    const logFile = path.join(deviceDir, `${date}.log`);
    
    if (!fs.existsSync(logFile)) {
        return res.status(404).json({ error: 'Log file not found' });
    }
    
    res.download(logFile, `${device}-${date}.log`);
});

// Receive logs from phones
app.post('/logs', (req, res) => {
    try {
        const { device, timestamp, uptime_sec, tailscale_ip, rssi, logs } = req.body;
        
        if (!device || !logs) {
            return res.status(400).json({ error: 'Missing device or logs' });
        }
        
        const deviceId = sanitizeDeviceId(device);
        const deviceDir = path.join(LOG_DIR, deviceId);
        
        // Create device directory if it doesn't exist
        if (!fs.existsSync(deviceDir)) {
            fs.mkdirSync(deviceDir, { recursive: true });
        }
        
        // Get today's log file
        const dateStr = getDateString();
        const logFile = path.join(deviceDir, `${dateStr}.log`);
        
        // Format log entry with metadata
        const now = new Date().toISOString();
        const clientIp = req.ip || req.connection.remoteAddress;
        
        let entry = `\n=== ${now} | uptime: ${uptime_sec}s | rssi: ${rssi}dBm | from: ${clientIp}`;
        if (tailscale_ip) {
            entry += ` | ts_ip: ${tailscale_ip}`;
        }
        entry += ` ===\n`;
        entry += logs.replace(/\\n/g, '\n').replace(/\\r/g, '');
        
        // Append to log file
        fs.appendFileSync(logFile, entry);
        
        // Check and rotate if needed
        rotateLogsIfNeeded(deviceDir);
        
        console.log(`📥 Received ${logs.length} bytes from ${deviceId}`);
        
        res.json({ 
            status: 'ok', 
            device: deviceId,
            bytes_received: logs.length,
            log_file: `${dateStr}.log`
        });
    } catch (err) {
        console.error('Error processing logs:', err);
        res.status(500).json({ error: err.message });
    }
});

// Simple web UI for viewing logs
app.get('/', (req, res) => {
    res.send(`
<!DOCTYPE html>
<html>
<head>
    <title>Phone Log Receiver</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; margin: 0; padding: 20px; }
        .container { max-width: 800px; margin: auto; }
        h1 { color: #e94560; }
        .device { background: #16213e; padding: 15px; border-radius: 8px; margin: 10px 0; border-left: 3px solid #e94560; }
        .device h3 { margin: 0 0 10px; color: #4ade80; }
        .stats { color: #888; font-size: 14px; }
        a { color: #e94560; text-decoration: none; }
        a:hover { text-decoration: underline; }
        .empty { color: #666; text-align: center; padding: 40px; }
        pre { background: #0f0f23; padding: 15px; border-radius: 6px; overflow-x: auto; font-size: 12px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📱 Phone Log Receiver</h1>
        <p>Endpoint: <code>POST /logs</code></p>
        <div id="devices"></div>
    </div>
    <script>
        fetch('/devices')
            .then(r => r.json())
            .then(data => {
                const container = document.getElementById('devices');
                if (!data.devices || data.devices.length === 0) {
                    container.innerHTML = '<div class="empty">No devices have sent logs yet.</div>';
                    return;
                }
                container.innerHTML = data.devices.map(d => \`
                    <div class="device">
                        <h3>📞 \${d.device}</h3>
                        <div class="stats">
                            \${d.files} log file(s) · \${d.totalSizeMB} MB total
                        </div>
                        <p><a href="/logs/\${d.device}">View latest logs</a></p>
                    </div>
                \`).join('');
            });
    </script>
</body>
</html>
    `);
});

// Helper functions
function sanitizeDeviceId(device) {
    // Only allow alphanumeric, dash, underscore
    return device.replace(/[^a-zA-Z0-9_-]/g, '_').substring(0, 64);
}

function getDateString() {
    const now = new Date();
    return now.toISOString().split('T')[0]; // YYYY-MM-DD
}

function rotateLogsIfNeeded(deviceDir) {
    try {
        const files = fs.readdirSync(deviceDir)
            .filter(f => f.endsWith('.log'))
            .sort();
        
        // Delete old logs
        const cutoffDate = new Date();
        cutoffDate.setDate(cutoffDate.getDate() - LOG_RETENTION_DAYS);
        const cutoffStr = cutoffDate.toISOString().split('T')[0];
        
        for (const file of files) {
            const dateStr = file.replace('.log', '');
            if (dateStr < cutoffStr) {
                fs.unlinkSync(path.join(deviceDir, file));
                console.log(`🗑️ Deleted old log: ${deviceDir}/${file}`);
            }
        }
        
        // Check total size
        let totalSize = 0;
        for (const file of files) {
            const filePath = path.join(deviceDir, file);
            if (fs.existsSync(filePath)) {
                totalSize += fs.statSync(filePath).size;
            }
        }
        
        // Delete oldest files if over size limit
        const maxBytes = MAX_LOG_SIZE_MB * 1024 * 1024;
        const sortedFiles = files.sort(); // Oldest first
        
        while (totalSize > maxBytes && sortedFiles.length > 1) {
            const oldestFile = sortedFiles.shift();
            const filePath = path.join(deviceDir, oldestFile);
            if (fs.existsSync(filePath)) {
                const fileSize = fs.statSync(filePath).size;
                fs.unlinkSync(filePath);
                totalSize -= fileSize;
                console.log(`🗑️ Deleted oversized log: ${deviceDir}/${oldestFile}`);
            }
        }
    } catch (err) {
        console.error('Error rotating logs:', err);
    }
}

// Start server
app.listen(PORT, '0.0.0.0', () => {
    console.log(`📡 Phone Log Receiver running on port ${PORT}`);
    console.log(`   Log directory: ${LOG_DIR}`);
    console.log(`   Retention: ${LOG_RETENTION_DAYS} days`);
    console.log(`   Max size per device: ${MAX_LOG_SIZE_MB} MB`);
});

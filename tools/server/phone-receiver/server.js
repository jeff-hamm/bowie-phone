const express = require('express');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const net = require('net');

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

// ── Session helpers ──────────────────────────────────────────────────────────

// Derive a short session id from the device's boot_id (random hex generated per boot).
// Falls back to a server-generated id when the device doesn't supply one.
function getSessionId(bootId) {
    if (bootId) return sanitizeDeviceId(bootId).substring(0, 12);
    return crypto.randomBytes(6).toString('hex');
}

// Resolve (or create) the current session dir for a device + boot_id.
// Layout:  <LOG_DIR>/<device>/<session>/
//   <session>__metadata.env   — written once on first POST
//   <session>_<YYYY-MM-DD>.log — one per calendar day
function resolveSession(deviceId, bootId, meta) {
    const deviceDir = path.join(LOG_DIR, deviceId);
    if (!fs.existsSync(deviceDir)) fs.mkdirSync(deviceDir, { recursive: true });

    const sessionId = getSessionId(bootId);
    const sessionDir = path.join(deviceDir, sessionId);
    if (!fs.existsSync(sessionDir)) fs.mkdirSync(sessionDir, { recursive: true });

    const metaFile = path.join(sessionDir, `${sessionId}__metadata.env`);
    if (!fs.existsSync(metaFile)) {
        const lines = [
            `SESSION_ID=${sessionId}`,
            `BOOT_ID=${bootId || 'unknown'}`,
            `DEVICE=${deviceId}`,
            `FIRST_SEEN=${new Date().toISOString()}`,
            `CLIENT_IP=${meta.clientIp || ''}`,
            `TAILSCALE_IP=${meta.tailscaleIp || ''}`,
            `FIRMWARE=${meta.firmware || ''}`,
            `BOOT_REASON=${meta.bootReason || ''}`,
        ];
        fs.writeFileSync(metaFile, lines.join('\n') + '\n');
    }
    return { sessionId, sessionDir, metaFile };
}

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
                const sessions = fs.readdirSync(deviceDir)
                    .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory());
                let totalSize = 0;
                let totalFiles = 0;
                for (const s of sessions) {
                    const sDir = path.join(deviceDir, s);
                    const logs = fs.readdirSync(sDir).filter(f => f.endsWith('.log'));
                    totalFiles += logs.length;
                    totalSize += logs.reduce((sum, f) =>
                        sum + fs.statSync(path.join(sDir, f)).size, 0);
                }
                return {
                    device,
                    sessions: sessions.length,
                    files: totalFiles,
                    totalSizeBytes: totalSize,
                    totalSizeMB: (totalSize / 1024 / 1024).toFixed(2)
                };
            });
        res.json({ devices });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Get logs for a specific device (latest session by default)
app.get('/logs/:device', (req, res) => {
    const { device } = req.params;
    const { date, lines = 100, session } = req.query;

    const deviceDir = path.join(LOG_DIR, sanitizeDeviceId(device));
    if (!fs.existsSync(deviceDir)) {
        return res.status(404).json({ error: 'Device not found' });
    }

    try {
        // Resolve session: use query param, or find the most-recently-written session
        const sessions = fs.readdirSync(deviceDir)
            .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory())
            .sort((a, b) => {
                const sa = fs.statSync(path.join(deviceDir, a)).mtimeMs;
                const sb = fs.statSync(path.join(deviceDir, b)).mtimeMs;
                return sb - sa;
            });
        if (sessions.length === 0) {
            return res.status(404).json({ error: 'No sessions for device' });
        }
        const sessionId = session ? sanitizeDeviceId(session) : sessions[0];
        const sessionDir = path.join(deviceDir, sessionId);
        if (!fs.existsSync(sessionDir)) {
            return res.status(404).json({ error: 'Session not found', available_sessions: sessions });
        }

        const targetDate = date || getDateString();
        const logFile = path.join(sessionDir, `${sessionId}_${targetDate}.log`);

        if (!fs.existsSync(logFile)) {
            const files = fs.readdirSync(sessionDir)
                .filter(f => f.endsWith('.log'))
                .map(f => f.replace(`${sessionId}_`, '').replace('.log', ''));
            return res.status(404).json({ error: 'No logs for this date', session: sessionId, available_dates: files });
        }

        const content = fs.readFileSync(logFile, 'utf8');
        const allLines = content.split('\n').filter(l => l.trim());
        const lastLines = allLines.slice(-parseInt(lines));

        // Include metadata if it exists
        let metadata = null;
        const metaFile = path.join(sessionDir, `${sessionId}__metadata.env`);
        if (fs.existsSync(metaFile)) {
            metadata = fs.readFileSync(metaFile, 'utf8');
        }

        res.json({
            device,
            session: sessionId,
            date: targetDate,
            total_lines: allLines.length,
            returned_lines: lastLines.length,
            metadata,
            logs: lastLines
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Download raw log file
app.get('/logs/:device/download/:date', (req, res) => {
    const { device, date } = req.params;
    const { session } = req.query;
    const deviceDir = path.join(LOG_DIR, sanitizeDeviceId(device));

    // Find session (latest if not specified)
    const sessions = fs.existsSync(deviceDir) ? fs.readdirSync(deviceDir)
        .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory())
        .sort((a, b) => fs.statSync(path.join(deviceDir, b)).mtimeMs - fs.statSync(path.join(deviceDir, a)).mtimeMs) : [];

    const sessionId = session ? sanitizeDeviceId(session) : (sessions[0] || '');
    const sessionDir = path.join(deviceDir, sessionId);
    const logFile = path.join(sessionDir, `${sessionId}_${date}.log`);

    if (!fs.existsSync(logFile)) {
        return res.status(404).json({ error: 'Log file not found' });
    }

    res.download(logFile, `${device}-${sessionId}-${date}.log`);
});

// Receive logs from phones
app.post('/logs', (req, res) => {
    try {
        const { device, timestamp, uptime_sec, tailscale_ip, rssi, logs,
                boot_id, boot_reason, firmware, boot } = req.body;

        if (!device || !logs) {
            return res.status(400).json({ error: 'Missing device or logs' });
        }

        const deviceId = sanitizeDeviceId(device);
        const clientIp = req.ip || req.connection.remoteAddress;

        // Resolve session from boot_id
        const { sessionId, sessionDir } = resolveSession(deviceId, boot_id, {
            clientIp,
            tailscaleIp: tailscale_ip,
            firmware: firmware || '',
            bootReason: boot_reason || '',
        });

        const dateStr = getDateString();
        const logFile = path.join(sessionDir, `${sessionId}_${dateStr}.log`);

        // If this is a boot notification, write a compact boot marker
        if (boot) {
            const now = new Date().toISOString();
            const bootLine = `[${now}] | uptime ${uptime_sec || 0}s > *** BOOT: firmware=${firmware || '?'} reason=${boot_reason || '?'} rssi=${rssi || '?'}dBm ip=${clientIp} ***\n`;
            fs.appendFileSync(logFile, bootLine);
        }

        // Format each log line:  [timestamp] | uptime [uptime]s > [content]
        const now = new Date().toISOString();
        const uptimeTag = uptime_sec != null ? `${uptime_sec}s` : '?';
        const rawLines = logs.replace(/\\n/g, '\n').replace(/\\r/g, '');
        const formatted = rawLines.split('\n')
            .filter(l => l.trim())
            .map(line => `[${now}] | uptime ${uptimeTag} > ${line}`)
            .join('\n');

        if (formatted) {
            fs.appendFileSync(logFile, formatted + '\n');
        }

        // Rotate
        rotateLogsIfNeeded(path.join(LOG_DIR, deviceId));

        console.log(`${deviceId}/${sessionId} +${logs.length}b`);

        res.json({
            status: 'ok',
            device: deviceId,
            session: sessionId,
            bytes_received: logs.length,
            log_file: `${sessionId}_${dateStr}.log`
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
                            \${d.sessions} session(s) · \${d.files} log file(s) · \${d.totalSizeMB} MB total
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
        const cutoffDate = new Date();
        cutoffDate.setDate(cutoffDate.getDate() - LOG_RETENTION_DAYS);
        const cutoffStr = cutoffDate.toISOString().split('T')[0];
        const maxBytes = MAX_LOG_SIZE_MB * 1024 * 1024;

        // Collect all log files across sessions: { path, date, size, sessionDir }
        const sessions = fs.readdirSync(deviceDir)
            .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory());

        let allLogs = [];
        for (const s of sessions) {
            const sDir = path.join(deviceDir, s);
            const files = fs.readdirSync(sDir).filter(f => f.endsWith('.log'));
            for (const f of files) {
                const fp = path.join(sDir, f);
                // Filename: <session>_<YYYY-MM-DD>.log — extract date
                const dateMatch = f.match(/(\d{4}-\d{2}-\d{2})\.log$/);
                const dateStr = dateMatch ? dateMatch[1] : '9999-99-99';
                allLogs.push({ path: fp, date: dateStr, size: fs.statSync(fp).size, sessionDir: sDir, session: s });
            }
        }

        allLogs.sort((a, b) => a.date.localeCompare(b.date));

        // 1. Delete logs older than retention period
        for (const entry of allLogs) {
            if (entry.date < cutoffStr) {
                fs.unlinkSync(entry.path);
                entry.deleted = true;
            }
        }

        // 2. Delete oldest logs if over size limit
        let totalSize = allLogs.filter(e => !e.deleted).reduce((s, e) => s + e.size, 0);
        for (const entry of allLogs) {
            if (totalSize <= maxBytes) break;
            if (entry.deleted) continue;
            fs.unlinkSync(entry.path);
            totalSize -= entry.size;
            entry.deleted = true;
        }

        // 3. Clean up empty session dirs (delete metadata if no logs remain)
        for (const s of sessions) {
            const sDir = path.join(deviceDir, s);
            if (!fs.existsSync(sDir)) continue;
            const remaining = fs.readdirSync(sDir).filter(f => f.endsWith('.log'));
            if (remaining.length === 0) {
                // Remove metadata and the empty dir
                for (const f of fs.readdirSync(sDir)) {
                    fs.unlinkSync(path.join(sDir, f));
                }
                fs.rmdirSync(sDir);
            }
        }
    } catch (err) {
        console.error('Error rotating logs:', err);
    }
}

// ── Phone log intake ─────────────────────────────────────────────────────────
// Persistent TCP server that accepts outbound connections FROM phones.
// The phone sends a handshake: "BOWIE-LOG device=<id> boot=<bootId> firmware=<ver>\n"
// Server replies "BOWIE-ACK\n".  All subsequent data is raw log text, appended to
// the session log file.

const PHONE_INTAKE_PORT = parseInt(process.env.PHONE_INTAKE_PORT) || 2324;

// Track active phone intake connections: deviceId -> { socket, sessionDir, logStream }
const activePhoneConnections = new Map();

const phoneIntake = net.createServer((socket) => {
    const remoteAddr = socket.remoteAddress;
    console.log(`📡 Phone intake: connection from ${remoteAddr}`);

    let handshakeDone = false;
    let buf = '';
    let deviceId = null;
    let logFilePath = null;

    // Close if no handshake within 10 seconds
    const handshakeTimeout = setTimeout(() => {
        if (!handshakeDone) {
            console.log(`📡 Phone intake: handshake timeout from ${remoteAddr}`);
            socket.destroy();
        }
    }, 10000);

    socket.on('data', (data) => {
        if (!handshakeDone) {
            buf += data.toString();
            const nl = buf.indexOf('\n');
            if (nl < 0) return;

            clearTimeout(handshakeTimeout);
            const line = buf.substring(0, nl).trim();
            const remainder = buf.substring(nl + 1);

            // Parse: BOWIE-LOG device=<id> boot=<bootId> firmware=<ver>
            const match = line.match(/^BOWIE-LOG\s+device=(\S+)\s+boot=(\S+)(?:\s+firmware=(\S+))?$/);
            if (!match) {
                console.log(`📡 Phone intake: bad handshake from ${remoteAddr}: ${line}`);
                socket.write('ERROR bad handshake\n');
                socket.destroy();
                return;
            }

            deviceId = sanitizeDeviceId(match[1]);
            const bootId = match[2];
            const firmware = match[3] || 'unknown';

            // Create or reuse session
            const { sessionId, sessionDir } = resolveSession(deviceId, bootId, {
                clientIp: remoteAddr,
                firmware: firmware,
            });

            const dateStr = new Date().toISOString().split('T')[0];
            logFilePath = path.join(sessionDir, `${sessionId}_${dateStr}.log`);

            socket.write('BOWIE-ACK\n');
            handshakeDone = true;

            console.log(`📡 Phone intake: ${deviceId} (boot=${bootId}) connected, logging to ${logFilePath}`);

            // Store active connection
            activePhoneConnections.set(deviceId, { socket, sessionDir, logFilePath });

            // Write any remainder after the handshake line
            if (remainder.length > 0) {
                appendLogData(logFilePath, remainder);
            }
            return;
        }

        // Post-handshake: raw log data, append to file
        appendLogData(logFilePath, data.toString());
    });

    socket.on('error', (err) => {
        console.log(`📡 Phone intake: error from ${deviceId || remoteAddr}: ${err.message}`);
    });

    socket.on('close', () => {
        console.log(`📡 Phone intake: ${deviceId || remoteAddr} disconnected`);
        if (deviceId) {
            activePhoneConnections.delete(deviceId);
        }
        clearTimeout(handshakeTimeout);
    });
});

function appendLogData(filePath, data) {
    try {
        fs.appendFileSync(filePath, data);
    } catch (err) {
        console.error(`📡 Phone intake: write error: ${err.message}`);
    }
}

phoneIntake.listen(PHONE_INTAKE_PORT, '0.0.0.0', () => {
    console.log(`📡 Phone log intake listening on port ${PHONE_INTAKE_PORT}`);
});

// Expose active connections via HTTP for diagnostics
app.get('/intake/status', (req, res) => {
    const connections = [];
    for (const [device, info] of activePhoneConnections) {
        connections.push({
            device,
            remoteAddress: info.socket.remoteAddress,
            logFile: path.basename(info.logFilePath),
        });
    }
    res.json({ active_connections: connections });
});

// ── Server-initiated telnet monitor ──────────────────────────────────────────
// The server connects to the phone's telnet port and sends a "BOWIE-SERVER"
// handshake so the phone knows to suppress HTTP POSTs.  Works through NAT/proxies
// where IP-matching alone would fail.

// Track active server monitor connections: deviceId -> socket
const activeMonitorConnections = new Map();

app.post('/monitor/:device', (req, res) => {
    const deviceId = sanitizeDeviceId(req.params.device);
    if (activeMonitorConnections.has(deviceId)) {
        return res.json({ status: 'already_monitoring', device: deviceId });
    }
    const ip = resolveDeviceIp(deviceId);
    if (!ip) return res.status(404).json({ error: 'Device not found or no known IP' });

    const phone = net.createConnection({ host: ip, port: PHONE_TELNET_PORT, timeout: 5000 }, () => {
        // Send handshake so phone identifies us as the log server
        phone.write('BOWIE-SERVER\n');
        console.log(`📡 Monitor: connected to ${deviceId} @ ${ip}, sent BOWIE-SERVER handshake`);
        activeMonitorConnections.set(deviceId, phone);
        res.json({ status: 'monitoring', device: deviceId, phone_ip: ip });
    });

    const logFile = openProxyLog(deviceId);

    phone.on('data', (data) => {
        if (logFile) {
            try { fs.appendFileSync(logFile, data); } catch (_) {}
        }
    });

    phone.on('error', (err) => {
        console.log(`📡 Monitor: ${deviceId} error: ${err.message}`);
        activeMonitorConnections.delete(deviceId);
        if (!res.headersSent) {
            res.status(502).json({ error: `Connection failed: ${err.message}` });
        }
    });
    phone.on('timeout', () => {
        console.log(`📡 Monitor: ${deviceId} connection timeout`);
        phone.destroy();
        activeMonitorConnections.delete(deviceId);
        if (!res.headersSent) {
            res.status(504).json({ error: 'Connection timeout' });
        }
    });
    phone.on('close', () => {
        console.log(`📡 Monitor: ${deviceId} disconnected`);
        activeMonitorConnections.delete(deviceId);
    });
});

app.delete('/monitor/:device', (req, res) => {
    const deviceId = sanitizeDeviceId(req.params.device);
    const socket = activeMonitorConnections.get(deviceId);
    if (!socket) return res.status(404).json({ error: 'Not monitoring this device' });
    socket.destroy();
    activeMonitorConnections.delete(deviceId);
    res.json({ status: 'stopped', device: deviceId });
});

app.get('/monitor/status', (req, res) => {
    const monitors = [];
    for (const [device, socket] of activeMonitorConnections) {
        monitors.push({ device, remoteAddress: socket.remoteAddress });
    }
    res.json({ active_monitors: monitors });
});

// ── Telnet proxy ─────────────────────────────────────────────────────────────
// TCP server that bridges a desktop client to a phone's ESPTelnet (port 23).
// On connect the proxy looks up the most-recently-seen IP for the device from
// session metadata.  If only one device exists it auto-connects; otherwise the
// client sends "CONNECT <device-id>\r\n" as the first line.
// Everything relayed is also appended to the session log for persistence.

const TELNET_PROXY_PORT = parseInt(process.env.TELNET_PROXY_PORT) || 2323;
const PHONE_TELNET_PORT = parseInt(process.env.PHONE_TELNET_PORT) || 23;

// Look up the most recent Tailscale IP for a device from session metadata
function resolveDeviceIp(deviceId) {
    const deviceDir = path.join(LOG_DIR, deviceId);
    if (!fs.existsSync(deviceDir)) return null;

    const sessions = fs.readdirSync(deviceDir)
        .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory())
        .sort((a, b) => {
            const sa = fs.statSync(path.join(deviceDir, a)).mtimeMs;
            const sb = fs.statSync(path.join(deviceDir, b)).mtimeMs;
            return sb - sa;
        });

    for (const s of sessions) {
        const metaFile = path.join(deviceDir, s, `${s}__metadata.env`);
        if (!fs.existsSync(metaFile)) continue;
        const content = fs.readFileSync(metaFile, 'utf8');
        const match = content.match(/^TAILSCALE_IP=(.+)$/m);
        if (match && match[1].trim()) return match[1].trim();
        // Fall back to CLIENT_IP
        const cliMatch = content.match(/^CLIENT_IP=(.+)$/m);
        if (cliMatch && cliMatch[1].trim()) return cliMatch[1].trim();
    }
    return null;
}

// List all known device ids
function listDeviceIds() {
    if (!fs.existsSync(LOG_DIR)) return [];
    return fs.readdirSync(LOG_DIR)
        .filter(f => fs.statSync(path.join(LOG_DIR, f)).isDirectory());
}

// Open a log-appending stream for the proxy session
function openProxyLog(deviceId) {
    const deviceDir = path.join(LOG_DIR, deviceId);
    if (!fs.existsSync(deviceDir)) return null;
    const sessions = fs.readdirSync(deviceDir)
        .filter(f => fs.statSync(path.join(deviceDir, f)).isDirectory())
        .sort((a, b) => fs.statSync(path.join(deviceDir, b)).mtimeMs - fs.statSync(path.join(deviceDir, a)).mtimeMs);
    if (sessions.length === 0) return null;
    const s = sessions[0];
    const dateStr = new Date().toISOString().split('T')[0];
    return path.join(deviceDir, s, `${s}_${dateStr}.log`);
}

function bridgeToPhone(client, deviceId) {
    const ip = resolveDeviceIp(deviceId);
    if (!ip) {
        client.write(`ERROR: No known IP for device "${deviceId}"\r\n`);
        client.end();
        return;
    }

    client.write(`Connecting to ${deviceId} (${ip}:${PHONE_TELNET_PORT})...\r\n`);

    const phone = net.createConnection({ host: ip, port: PHONE_TELNET_PORT, timeout: 5000 }, () => {
        client.write(`Connected.\r\n`);
        console.log(`📡 Telnet proxy: bridged to ${deviceId} @ ${ip}`);
    });

    const logFile = openProxyLog(deviceId);

    // Phone → Client (also log)
    phone.on('data', (data) => {
        client.write(data);
        if (logFile) {
            const now = new Date().toISOString();
            const lines = data.toString('utf8').split('\n')
                .filter(l => l.trim())
                .map(l => `[${now}] | telnet-proxy > ${l}`)
                .join('\n');
            if (lines) {
                try { fs.appendFileSync(logFile, lines + '\n'); } catch (_) {}
            }
        }
    });

    // Client → Phone
    client.on('data', (data) => {
        phone.write(data);
    });

    phone.on('error', (err) => {
        client.write(`\r\nERROR: ${err.message}\r\n`);
        client.end();
    });
    phone.on('timeout', () => {
        client.write(`\r\nERROR: Connection to phone timed out\r\n`);
        phone.destroy();
        client.end();
    });
    phone.on('end', () => { client.end(); });
    client.on('end', () => { phone.destroy(); });
    client.on('error', () => { phone.destroy(); });
}

const telnetProxy = net.createServer((client) => {
    const devices = listDeviceIds();

    if (devices.length === 0) {
        client.write('No devices have connected yet.\r\n');
        client.end();
        return;
    }

    // Auto-connect if only one device
    if (devices.length === 1) {
        bridgeToPhone(client, devices[0]);
        return;
    }

    // Multiple devices — prompt for selection
    client.write('Available devices:\r\n');
    devices.forEach((d, i) => client.write(`  ${i + 1}. ${d}\r\n`));
    client.write('Type device name or number: ');

    let buf = '';
    const onData = (data) => {
        buf += data.toString();
        const nl = buf.indexOf('\n');
        if (nl < 0) return;

        client.removeListener('data', onData);
        const choice = buf.substring(0, nl).replace(/\r/g, '').trim();

        // Accept number or name
        const num = parseInt(choice);
        let deviceId;
        if (!isNaN(num) && num >= 1 && num <= devices.length) {
            deviceId = devices[num - 1];
        } else {
            deviceId = sanitizeDeviceId(choice);
        }

        if (!devices.includes(deviceId)) {
            client.write(`Unknown device: "${choice}"\r\n`);
            client.end();
            return;
        }

        bridgeToPhone(client, deviceId);
    };
    client.on('data', onData);
});

telnetProxy.listen(TELNET_PROXY_PORT, '0.0.0.0', () => {
    console.log(`📡 Telnet proxy listening on port ${TELNET_PROXY_PORT}`);
});

// HTTP endpoint for telnet connection info
app.get('/telnet/:device', (req, res) => {
    const deviceId = sanitizeDeviceId(req.params.device);
    const ip = resolveDeviceIp(deviceId);
    if (!ip) return res.status(404).json({ error: 'Device not found or no known IP' });
    res.json({
        device: deviceId,
        phone_ip: ip,
        phone_telnet_port: PHONE_TELNET_PORT,
        proxy_port: TELNET_PROXY_PORT,
        connect_via_proxy: `telnet <this-server> ${TELNET_PROXY_PORT}`,
        connect_direct: `telnet ${ip} ${PHONE_TELNET_PORT}`,
    });
});

// Start HTTP server
app.listen(PORT, '0.0.0.0', () => {
    console.log(`📡 Phone Log Receiver running on port ${PORT}`);
    console.log(`   Log directory: ${LOG_DIR}`);
    console.log(`   Retention: ${LOG_RETENTION_DAYS} days`);
    console.log(`   Max size per device: ${MAX_LOG_SIZE_MB} MB`);
});

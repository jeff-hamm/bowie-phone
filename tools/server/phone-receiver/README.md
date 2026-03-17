# Phone Log Receiver

A simple log aggregation server for Bowie Phone devices connected via WireGuard VPN.

## Quick Start

```bash
# Start the log receiver
docker-compose up -d

# View logs
docker-compose logs -f
```

## Configuration

Environment variables in `docker-compose.yml`:

| Variable | Default | Description |
|----------|---------|-------------|
| `PORT` | 3000 | HTTP server port |
| `LOG_DIR` | /app/logs | Directory to store logs |
| `LOG_RETENTION_DAYS` | 30 | Days to keep logs |
| `MAX_LOG_SIZE_MB` | 100 | Max log size per device |

## Phone Configuration

On your ESP32 phone, configure in `platformio.ini`:

```ini
build_flags =
    -DREMOTE_LOG_SERVER=\"http://10.253.0.1:3000/logs\"
    -DREMOTE_LOG_DEVICE_ID=\"kitchen-phone\"
```

Or configure via the web interface at `http://<phone-ip>/remotelog`

## API Endpoints

### `POST /logs`
Receive logs from phones.

**Request body:**
```json
{
    "device": "phone-ABC123",
    "timestamp": 123456789,
    "uptime_sec": 3600,
    "tailscale_ip": "100.64.0.100",
    "rssi": -45,
    "logs": "Log line 1\nLog line 2\n..."
}
```

### `GET /devices`
List all devices that have sent logs.

### `GET /logs/:device`
Get recent logs for a device.

Query params:
- `date`: Specific date (YYYY-MM-DD), defaults to today
- `lines`: Number of lines to return (default: 100)

### `GET /logs/:device/download/:date`
Download raw log file.

### `GET /health`
Health check endpoint.

## Log Storage

Logs are organized by device and date:
```
logs/
├── phone-ABC123/
│   ├── 2024-01-15.log
│   └── 2024-01-16.log
├── kitchen-phone/
│   └── 2024-01-16.log
└── living-room/
    └── 2024-01-16.log
```

## Optional: Log Viewer

Start with the Dozzle log viewer:

```bash
docker-compose --profile viewer up -d
```

Access at `http://localhost:8080`

## WireGuard Network Setup

Make sure this container is accessible from your WireGuard network. Example setup:

1. Run on your WireGuard server (e.g., `10.253.0.1`)
2. Phones connect via VPN and send to `http://10.253.0.1:3000/logs`
3. Logs are stored persistently in `./logs` directory

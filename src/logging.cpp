#include "logging.h"
#include <Arduino.h>

// Global logger instance
LoggerClass Logger;

LoggerClass::LoggerClass() : serialPrint(nullptr), logIndex(0), logCount(0), bufferPos(0) {
    messageBuffer[0] = '\0';
}

void LoggerClass::addLogger(Print& print) {
    serialPrint = &print;
}

size_t LoggerClass::write(uint8_t byte) {
    // Write to serial/print object (if available)
    size_t result = 0;
    if (serialPrint) {
        result = serialPrint->write(byte);
    }
    
    // Buffer the character for log messages
    if (bufferPos < MAX_LOG_MESSAGE_LENGTH - 1) {
        messageBuffer[bufferPos++] = byte;
        messageBuffer[bufferPos] = '\0';
        
        // If we hit a newline, add the message to log buffer
        if (byte == '\n' || byte == '\r') {
            if (bufferPos > 1) { // Only if there's actual content (more than just the newline)
                // Remove the newline from the buffer
                messageBuffer[bufferPos - 1] = '\0';
                
                // Add timestamped message to log buffer
                String timestampedMessage = String(millis()) + "ms: " + String(messageBuffer);
                addMessageToBuffer(timestampedMessage);
            }
            
            // Reset buffer
            bufferPos = 0;
            messageBuffer[0] = '\0';
        }
    } else {
        // Buffer full, add what we have and reset
        String timestampedMessage = String(millis()) + "ms: " + String(messageBuffer);
        addMessageToBuffer(timestampedMessage);
        
        // Reset buffer and add current byte
        bufferPos = 0;
        messageBuffer[bufferPos++] = byte;
        messageBuffer[bufferPos] = '\0';
    }
    
    return result;
}

size_t LoggerClass::write(const uint8_t* buffer, size_t size) {
    size_t result = 0;
    if (serialPrint) {
        result = serialPrint->write(buffer, size);
    }
    
    // Process each byte for log buffer (without calling serial again)
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = buffer[i];
        
        // Buffer the character for log messages
        if (bufferPos < MAX_LOG_MESSAGE_LENGTH - 1) {
            messageBuffer[bufferPos++] = byte;
            messageBuffer[bufferPos] = '\0';
            
            // If we hit a newline, add the message to log buffer
            if (byte == '\n' || byte == '\r') {
                if (bufferPos > 1) { // Only if there's actual content (more than just the newline)
                    // Remove the newline from the buffer
                    messageBuffer[bufferPos - 1] = '\0';
                    
                    // Add timestamped message to log buffer
                    String timestampedMessage = String(millis()) + "ms: " + String(messageBuffer);
                    addMessageToBuffer(timestampedMessage);
                }
                
                // Reset buffer
                bufferPos = 0;
                messageBuffer[0] = '\0';
            }
        } else {
            // Buffer full, add what we have and reset
            String timestampedMessage = String(millis()) + "ms: " + String(messageBuffer);
            addMessageToBuffer(timestampedMessage);
            
            // Reset buffer and add current byte
            bufferPos = 0;
            messageBuffer[bufferPos++] = byte;
            messageBuffer[bufferPos] = '\0';
        }
    }
    
    return result;
}

void LoggerClass::addMessageToBuffer(const String& message) {
    // Add to circular buffer
    logBuffer[logIndex] = message;
    logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
    
    if (logCount < LOG_BUFFER_SIZE) {
        logCount++;
    }
}

String LoggerClass::getLogsAsHtml() {
    String html = R"(
<!DOCTYPE html><html><head><title>System Logs</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="5">
<style>
body{font-family:monospace;margin:10px;background:#000;color:#0f0}
.header{background:#333;color:#fff;padding:10px;margin-bottom:10px;border-radius:3px}
.log{background:#111;padding:5px;margin:2px 0;border-left:3px solid #0f0;font-size:12px;word-wrap:break-word}
.nav{background:#444;padding:10px;margin-bottom:10px;text-align:center}
.nav a{color:#0ff;text-decoration:none;margin:0 10px}
.stats{background:#222;color:#fff;padding:5px;margin:5px 0;font-size:11px}
</style></head><body>
<div class="header"><h2>📱 System Logs</h2></div>
<div class="nav">
<a href="/">🏠 Home</a> | <a href="/logs">🔄 Refresh</a>
</div>
<div class="stats">Total Messages: )";

    html += String(logCount);
    html += " | Buffer: " + String(LOG_BUFFER_SIZE) + " | Free RAM: " + String(ESP.getFreeHeap()) + " bytes</div>";
    
    // Add log messages (newest first)
    if (logCount > 0) {
        int start = logCount < LOG_BUFFER_SIZE ? 0 : logIndex;
        for (int i = 0; i < logCount; i++) {
            int idx = (start + logCount - 1 - i) % LOG_BUFFER_SIZE;
            html += "<div class='log'>" + logBuffer[idx] + "</div>";
        }
    } else {
        html += "<div class='log'>No log messages yet...</div>";
    }
    
    html += "</body></html>";
    return html;
}

String LoggerClass::getLogsAsJson() {
    String json = "{\"logs\":[";
    
    if (logCount > 0) {
        int start = logCount < LOG_BUFFER_SIZE ? 0 : logIndex;
        for (int i = 0; i < logCount; i++) {
            int idx = (start + logCount - 1 - i) % LOG_BUFFER_SIZE;
            if (i > 0) json += ",";
            json += "\"" + logBuffer[idx] + "\"";
        }
    }
    
    json += "],\"count\":" + String(logCount) + ",\"freeRam\":" + String(ESP.getFreeHeap()) + "}";
    return json;
}

void LoggerClass::clearLogs() {
    logIndex = 0;
    logCount = 0;
    bufferPos = 0;
    messageBuffer[0] = '\0';
}

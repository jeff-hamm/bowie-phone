#ifndef LOGGING_H
#define LOGGING_H

#include <Print.h>
#include <WString.h>

#define LOG_BUFFER_SIZE 100
#define MAX_LOG_MESSAGE_LENGTH 256
#define MAX_LOG_STREAMS 3  // Serial + Telnet + future expansion

// ============================================================================
// LOG LEVEL CONFIGURATION
// ============================================================================

enum LogLevel {
    LOG_QUIET = 0,   // No output
    LOG_NORMAL = 1,  // Standard output
    LOG_DEBUG = 2    // Verbose debug output
};

// Default log level based on compile-time flags
#if defined(DEBUG)
    #define DEFAULT_LOG_LEVEL LOG_DEBUG
#elif defined(QUIET)
    #define DEFAULT_LOG_LEVEL LOG_QUIET
#else
    #define DEFAULT_LOG_LEVEL LOG_NORMAL
#endif

class LoggerClass : public Print {
private:
    Print* streams[MAX_LOG_STREAMS];  // Multiple output streams (Serial, Telnet, etc.)
    int streamCount;
    String logBuffer[LOG_BUFFER_SIZE];
    int logIndex;
    int logCount;
    char messageBuffer[MAX_LOG_MESSAGE_LENGTH];
    int bufferPos;
    LogLevel currentLogLevel;

public:
    LoggerClass();
    
    // Add output stream (Serial, Telnet, etc.)
    void addLogger(Print& print);
    
    // Remove a specific stream
    void removeLogger(Print& print);
    
    // Log level management
    void setLogLevel(LogLevel level) { currentLogLevel = level; }
    LogLevel getLogLevel() const { return currentLogLevel; }
    
    // Print interface implementation
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    
    // Debug-level logging (only outputs when logLevel >= LOG_DEBUG)
    void debug(const char* message);
    void debugln(const char* message);
    void debugln();
    void debugf(const char* format, ...);
    
    // Get log messages for web interface
    String getLogsAsHtml();
    String getLogsAsJson();
    
    // Buffer management
    void clearLogs();
    int getLogCount() const { return logCount; }
    
private:
    void addMessageToBuffer(const String& message);
};

// Global logger instance (declared in logging.cpp)  
extern LoggerClass Logger;

#endif // LOGGING_H

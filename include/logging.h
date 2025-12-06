#ifndef LOGGING_H
#define LOGGING_H

#include <Print.h>
#include <WString.h>

#define LOG_BUFFER_SIZE 100
#define MAX_LOG_MESSAGE_LENGTH 256

class LoggerClass : public Print {
private:
    Print* serialPrint;  // Reference to Serial or other Print object
    String logBuffer[LOG_BUFFER_SIZE];
    int logIndex;
    int logCount;
    char messageBuffer[MAX_LOG_MESSAGE_LENGTH];
    int bufferPos;

public:
    LoggerClass();
    
    // Initialize with a Print object (replaces constructor functionality)
    void addLogger(Print& print);
    
    // Print interface implementation
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    
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

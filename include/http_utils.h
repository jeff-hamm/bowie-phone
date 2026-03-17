#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <SD_MMC.h>
#include <Update.h>
#include "config.h"
#include "logging.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

#ifndef USER_AGENT_HEADER
#define USER_AGENT_HEADER "BowiePhone/" FIRMWARE_VERSION
#endif

// Helper macros for SD vs SD_MMC abstraction
#if SD_USE_MMC
  #define HTTP_SD_OPEN(path, mode) SD_MMC.open(path, mode)
#else
  #define HTTP_SD_OPEN(path, mode) SD.open(path, mode)
#endif

// ============================================================================
// HttpClient — thin wrapper around HTTPClient + WiFiClientSecure
// ============================================================================
//
// Owns both objects and applies project defaults (User-Agent, insecure TLS,
// follow-redirects, timeout from config.h).  Never allocates on the heap;
// create on the stack and let it destruct naturally.
//
// Typical use:
//   HttpClient http;                        // 15 s default timeout
//   if (http.get(url)) {
//       String body = http.getString();     // calls end() internally
//   }
//
//   HttpClient http(HTTP_TIMEOUT_SHORT_MS); // 5 s timeout
//   if (http.post(url, json)) { ... }       // uses persistent Content-Type
//
//   int bytes = http.getFile(url, "/audio/song.mp3");

class HttpClient {
public:
    // Per-request header passed to get()/post() overloads
    struct Header {
        const char* name;
        const char* value;
    };

    // Construct with optional timeout override (default HTTP_TIMEOUT_MS from config.h)
    explicit HttpClient(int timeoutMs = HTTP_TIMEOUT_MS, const Header* headers = nullptr, size_t headerCount = 0)
        : _statusCode(0), _timeoutMs(timeoutMs), _headerCount(0) {
        _secure.setInsecure();
        _http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (headers) {
            for (size_t i = 0; i < headerCount && i < MAX_HEADERS; i++) {
                setPersistentHeader(headers[i].name, headers[i].value);
            }
        }
    }

    ~HttpClient() { end(); }

    // -- configuration -------------------------------------------------------

    // Add a persistent request header (re-applied on every request)
    void setPersistentHeader(const char* name, const char* value) {
        for (int i = 0; i < _headerCount; i++) {
            if (_headers[i].name.equalsIgnoreCase(name)) {
                _headers[i].value = value;
                return;
            }
        }
        if (_headerCount < MAX_HEADERS) {
            _headers[_headerCount].name = name;
            _headers[_headerCount].value = value;
            _headerCount++;
        }
    }
    void setPersistentHeader(const char* name, const String& value) {
        setPersistentHeader(name, value.c_str());
    }

    // Change the request timeout
    void setTimeout(int ms) { _timeoutMs = ms; }

    // Register response headers you want to read after the request.
    // Persists across requests on the same instance.
    void collectHeaders(const char* headerKeys[], size_t count) {
        _http.collectHeaders(headerKeys, count);
    }

    // Read a collected response header (call after get/post)
    String header(const char* name) { return _http.header(name); }

    // -- requests ------------------------------------------------------------

    // HTTP GET — returns true on 2xx
    bool get(const char* url) { return request(url, "GET"); }
    bool get(const String& url) { return get(url.c_str()); }
    bool get(const char* url, const Header* headers, size_t count) {
        return request(url, "GET", nullptr, headers, count);
    }

    // HTTP POST — returns true on 2xx.
    // If contentType is non-null it overrides the persistent Content-Type for
    // this request only.  Pass nullptr to use the persistent value.
    bool post(const char* url, const String& body,
              const char* contentType = nullptr) {
        if (contentType) {
            Header ct = {"Content-Type", contentType};
            return request(url, "POST", body.c_str(), &ct, 1);
        }
        return request(url, "POST", body.c_str());
    }
    bool post(const String& url, const String& body,
              const char* contentType = nullptr) {
        return post(url.c_str(), body, contentType);
    }
    bool post(const char* url, const String& body,
              const Header* headers, size_t count) {
        return request(url, "POST", body.c_str(), headers, count);
    }

    // -- response accessors --------------------------------------------------

    int statusCode() const { return _statusCode; }
    const String& statusMessage() const { return _statusMsg; }
    bool ok() const { return _statusCode >= 200 && _statusCode < 300; }

    // Read the full response body as a String.  Calls end() internally.
    String getString() {
        String s = _http.getString();
        end();
        return s;
    }

    // Response content length (-1 if unknown / chunked)
    int getSize() { return _http.getSize(); }

    // -- streaming accessors (use after get/post, before end) ----------------

    // Raw stream pointer for chunked / large-body reads.
    // Caller must call end() when done.
    WiFiClient* getStream() { return _http.getStreamPtr(); }

    // True while the underlying TCP connection is alive
    bool connected() { return _http.connected(); }

    // Finish the request and release the TCP connection
    void end() { _http.end(); }

    // Stream the response body through a callback after a successful get/post.
    // Callback: bool cb(const uint8_t* buf, size_t len) — return false to stop.
    // Calls end() when done.  Returns total bytes passed to the callback.
    template<typename Fn>
    int streamBody(Fn cb) {
        int contentLength = _http.getSize();
        WiFiClient* stream = _http.getStreamPtr();
        uint8_t buf[1024];
        int totalBytes = 0;

        while (_http.connected() && (contentLength > 0 || contentLength == -1)) {
            size_t avail = stream->available();
            if (avail) {
                size_t toRead = min(avail, sizeof(buf));
                int bytesRead = stream->readBytes(buf, toRead);
                if (bytesRead > 0) {
                    if (!cb((const uint8_t*)buf, (size_t)bytesRead)) break;
                    totalBytes += bytesRead;
                    if (contentLength > 0) contentLength -= bytesRead;
                }
            } else {
                delay(1);
            }
        }

        end();
        return totalBytes;
    }

    // -- convenience: download a URL and flash via Update --------------------
    // GETs `url`, streams the response into Update.write(), and calls
    // Update.end().  Returns bytes written, or -1 on any error.
    // Does NOT call esp_restart() — the caller decides when to reboot.
    // Caller should shut down SD / audio / WDT before calling this.
    int getUpdate(const char* url, int timeoutMs = HTTP_TIMEOUT_OTA_MS) {
        setTimeout(timeoutMs);
        if (!get(url)) return -1;

        int contentLength = getSize();
        if (contentLength <= 0) {
            setStatus(-1, "❌ OTA: Invalid content length");
            end();
            return -1;
        }

        Logger.printf("📦 OTA: Firmware size: %d bytes\n", contentLength);

        if (!Update.begin(contentLength)) {
            setStatus(-1, "❌ OTA: Not enough space: %s", Update.errorString());
            end();
            return -1;
        }

        bool writeError = false;
        size_t totalWritten = 0;
        int lastPercent = -1;

        streamBody([&](const uint8_t* buf, size_t len) -> bool {
            size_t w = Update.write(const_cast<uint8_t*>(buf), len);
            if (w != len) {
                setStatus(-1, "❌ OTA: Write failed: %s", Update.errorString());
                Update.abort();
                writeError = true;
                return false;
            }
            totalWritten += w;
            int pct = (totalWritten * 100) / contentLength;
            if (pct != lastPercent && pct % 10 == 0) {
                Logger.printf("📤 OTA Progress: %d%%\n", pct);
                lastPercent = pct;
            }
            return true;
        });

        if (writeError) return -1;

        if (!Update.end(true)) {
            setStatus(-1, "❌ OTA: End failed: %s", Update.errorString());
            return -1;
        }

        Logger.printf("✅ OTA: Complete (%u bytes)\n", (unsigned)totalWritten);
        return (int)totalWritten;
    }

    // -- convenience: download a URL directly to an SD card file -------------
    // Returns bytes written, or -1 on HTTP/file error.
    // The file is created (or overwritten) at `sdPath`.
    int getFile(const char* url, const char* sdPath,
                int timeoutMs = HTTP_TIMEOUT_DOWNLOAD_MS) {
        setTimeout(timeoutMs);
        if (!get(url)) return -1;

        File f = HTTP_SD_OPEN(sdPath, FILE_WRITE);
        if (!f) {
            setStatus(-1, "❌ Cannot create file: %s", sdPath);
            end();
            return -1;
        }

        int total = streamBody([&](const uint8_t* buf, size_t len) {
            f.write(buf, len);
            return true;
        });

        f.close();
        if (total > 0) Logger.printf("✅ Downloaded %d bytes to: %s\n", total, sdPath);
        return total;
    }

    // Access the underlying HTTPClient for edge cases (e.g. errorToString)
    HTTPClient& raw() { return _http; }

private:
    HTTPClient _http;
    WiFiClientSecure _secure;
    int _statusCode;
    int _timeoutMs;
    String _statusMsg;

    // -- persistent headers --------------------------------------------------
    static constexpr int MAX_HEADERS = 8;
    struct StoredHeader {
        String name;
        String value;
    };
    StoredHeader _headers[MAX_HEADERS];
    int _headerCount;

    // Apply persistent + per-request headers to _http
    void applyHeaders(const Header* extra = nullptr, size_t extraCount = 0) {
        _http.addHeader("User-Agent", USER_AGENT_HEADER);
        for (int i = 0; i < _headerCount; i++) {
            _http.addHeader(_headers[i].name, _headers[i].value);
        }
        if (extra) {
            for (size_t i = 0; i < extraCount; i++) {
                _http.addHeader(extra[i].name, extra[i].value);
            }
        }
    }

    // Begin a URL (HTTPS via _secure, HTTP via plain begin)
    bool beginUrl(const char* url) {
        _http.setTimeout(_timeoutMs);
        if (strncmp(url, "https", 5) == 0) {
            return _http.begin(_secure, url);
        }
        return _http.begin(url);
    }

    // Core request implementation
    bool request(const char* url, const char* method,
                 const char* body = nullptr,
                 const Header* headers = nullptr,
                 size_t headerCount = 0) {
        if (!beginUrl(url)) {
            setStatus(-1, "❌ HTTP begin failed for %s", url);
            return false;
        }
        applyHeaders(headers, headerCount);

        if (body) {
            _statusCode = _http.sendRequest(method, body);
        } else {
            _statusCode = _http.sendRequest(method);
        }

        if (_statusCode <= 0) {
            setStatus(_statusCode, "❌ HTTP error %d: %s", _statusCode,
                      _http.errorToString(_statusCode).c_str());
            end();
            return false;
        }
        if (_statusCode >= 400) {
            setStatus(_statusCode, "❌ HTTP %d for %s", _statusCode, url);
            end();
            return false;
        }
        _statusMsg = "";
        return true;
    }

    // Printf-style status setter that also logs
    void __attribute__((format(printf, 3, 4)))
    setStatus(int code, const char* fmt, ...) {
        _statusCode = code;
        char buf[192];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        _statusMsg = buf;
        Logger.println(buf);
    }
};

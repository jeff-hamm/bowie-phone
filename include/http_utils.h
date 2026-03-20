#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <SD_MMC.h>
#include <Update.h>
#include "config.h"
#include "logging.h"
#include "mbedtls/platform.h"
#include "esp_heap_caps.h"
#include <freertos/task.h>

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
        _http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (headers) {
            for (size_t i = 0; i < headerCount && i < MAX_HEADERS; i++) {
                setPersistentHeader(headers[i].name, headers[i].value);
            }
        }
    }

    ~HttpClient() { end(); delete _ownSecure; _ownSecure = nullptr; }

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

    // Force this instance to use a private WiFiClientSecure instead of the
    // shared singleton.  Required for HttpClient instances created off the
    // main task (e.g. a core-0 download task running alongside Goertzel).
    void useOwnSecure() {
        if (!_ownSecure) {
            mbedtls_platform_set_calloc_free(
                [](size_t n, size_t sz) -> void* {
                    void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_8BIT);
                },
                [](void* p) { heap_caps_free(p); }
            );
            _ownSecure = new WiFiClientSecure();
            _ownSecure->setInsecure();
        }
    }

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

    // -- non-blocking chunk reader for cooperative downloads -----------------

    // Initialise chunked reading state after a successful get().
    // Call readChunk() repeatedly until bodyDone() returns true, then end().
    void beginChunkedRead() { _bodyRemaining = _http.getSize(); }

    // Read up to `maxBytes` of response body into `buf`.
    // Returns bytes read (0 if nothing available yet, negative on error).
    // Non-blocking: returns immediately when no data is buffered.
    int readChunk(uint8_t* buf, size_t maxBytes) {
        WiFiClient* s = _http.getStreamPtr();
        if (!s) return -1;
        size_t avail = s->available();
        if (avail == 0) {
            // Connection closed with nothing left → we're done
            if (!_http.connected()) return -1;
            return 0; // nothing available yet, try later
        }
        size_t toRead = min(avail, maxBytes);
        int n = s->readBytes(buf, toRead);
        if (n > 0 && _bodyRemaining > 0) _bodyRemaining -= n;
        return n;
    }

    // True when the full body has been received (known length) or the
    // server has closed the connection (chunked / unknown length).
    bool bodyDone() {
        if (_bodyRemaining == 0) return true;                // known length, all read
        if (_bodyRemaining < 0 && !_http.connected()) return true; // chunked, conn closed
        return false;
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

    // -- async downloads ---------------------------------------------------
    // Each method spawns a self-deleting FreeRTOS task.  The task creates its
    // own WiFiClientSecure (via useOwnSecure) so it never touches the shared
    // singleton.  SD access inside the callback is NOT automatically
    // serialised — hold any SD mutex before queuing a write from the callback.

    using AsyncFileCallback   = void(*)(int bytesWritten, void* userData);
    using AsyncStringCallback = void(*)(bool success, const String& body, void* userData);
    using AsyncPostCallback   = void(*)(bool success, int statusCode, void* userData);

    // Async file download: saves response body to sdPath, calls cb when done.
    static TaskHandle_t getFileAsync(
            const char* url, const char* sdPath,
            AsyncFileCallback cb = nullptr, void* userData = nullptr,
            int timeoutMs = HTTP_TIMEOUT_DOWNLOAD_MS,
            UBaseType_t priority = 1, BaseType_t core = 0)
    {
        struct State {
            char url[300]; char sdPath[128];
            int timeoutMs;
            AsyncFileCallback cb; void* userData;
        };
        auto* s = new State();
        strncpy(s->url,    url,    sizeof(s->url)    - 1); s->url[sizeof(s->url)-1]       = '\0';
        strncpy(s->sdPath, sdPath, sizeof(s->sdPath) - 1); s->sdPath[sizeof(s->sdPath)-1] = '\0';
        s->timeoutMs = timeoutMs; s->cb = cb; s->userData = userData;

        TaskHandle_t handle = nullptr;
        xTaskCreatePinnedToCore([](void* p) {
            auto* s = static_cast<State*>(p);
            HttpClient http(s->timeoutMs);
            http.useOwnSecure();
            int result = http.getFile(s->url, s->sdPath, s->timeoutMs);
            if (s->cb) s->cb(result, s->userData);
            delete s;
            vTaskDelete(nullptr);
        }, "HttpDL", 12288, s, priority, &handle, core);
        return handle;
    }

    // Async GET: fetches response body as a String, calls cb when done.
    static TaskHandle_t getStringAsync(
            const char* url,
            AsyncStringCallback cb, void* userData = nullptr,
            int timeoutMs = HTTP_TIMEOUT_CATALOG_MS,
            UBaseType_t priority = 1, BaseType_t core = 0)
    {
        struct State {
            char url[300]; int timeoutMs;
            AsyncStringCallback cb; void* userData;
        };
        auto* s = new State();
        strncpy(s->url, url, sizeof(s->url) - 1); s->url[sizeof(s->url)-1] = '\0';
        s->timeoutMs = timeoutMs; s->cb = cb; s->userData = userData;

        TaskHandle_t handle = nullptr;
        xTaskCreatePinnedToCore([](void* p) {
            auto* s = static_cast<State*>(p);
            HttpClient http(s->timeoutMs);
            http.useOwnSecure();
            bool ok = http.get(s->url);
            String body = ok ? http.getString() : String();
            if (s->cb) s->cb(ok, body, s->userData);
            delete s;
            vTaskDelete(nullptr);
        }, "HttpGet", 16384, s, priority, &handle, core);
        return handle;
    }

    // Async POST: sends body to url, calls cb with (success, statusCode).
    // Spawns a self-deleting FreeRTOS task on the specified core.
    // The body String is moved into the task state to avoid dangling refs.
    // For HTTP targets only — HTTPS would need useOwnSecure() + more stack.
    static TaskHandle_t postAsync(
            const char* url, String body,
            AsyncPostCallback cb = nullptr, void* userData = nullptr,
            const char* contentType = "application/json",
            const char* extraHeaderName = nullptr,
            const char* extraHeaderValue = nullptr,
            int timeoutMs = HTTP_TIMEOUT_SHORT_MS,
            UBaseType_t priority = 0, BaseType_t core = 0)
    {
        struct State {
            char url[256]; char contentType[48];
            char extraName[32]; char extraValue[64];
            String body;
            int timeoutMs;
            AsyncPostCallback cb; void* userData;
        };
        auto* s = new State();
        strncpy(s->url, url, sizeof(s->url) - 1); s->url[sizeof(s->url)-1] = '\0';
        strncpy(s->contentType, contentType ? contentType : "application/json",
                sizeof(s->contentType) - 1); s->contentType[sizeof(s->contentType)-1] = '\0';
        s->extraName[0] = '\0'; s->extraValue[0] = '\0';
        if (extraHeaderName) {
            strncpy(s->extraName, extraHeaderName, sizeof(s->extraName) - 1);
            s->extraName[sizeof(s->extraName)-1] = '\0';
        }
        if (extraHeaderValue) {
            strncpy(s->extraValue, extraHeaderValue, sizeof(s->extraValue) - 1);
            s->extraValue[sizeof(s->extraValue)-1] = '\0';
        }
        s->body = std::move(body);
        s->timeoutMs = timeoutMs; s->cb = cb; s->userData = userData;

        TaskHandle_t handle = nullptr;
        xTaskCreatePinnedToCore([](void* p) {
            auto* s = static_cast<State*>(p);
            HttpClient http(s->timeoutMs);
            if (s->extraName[0])
                http.setPersistentHeader(s->extraName, s->extraValue);
            bool ok = http.post(s->url, s->body, s->contentType);
            int code = http.statusCode();
            s->body = String();  // free before callback
            if (s->cb) s->cb(ok, code, s->userData);
            delete s;
            vTaskDelete(nullptr);
        }, "HttpPost", 8192, s, priority, &handle, core);
        return handle;
    }

    // =========================================================================
    // Cooperative (tickable) requests — run on core 1, caller calls tick()
    // =========================================================================
    //
    // Usage:
    //   auto* req = HttpClient::chunkedGet(url, onDone, userData);
    //   // in loop():
    //   if (req) { req->tick(); if (req->done()) { delete req; req = nullptr; } }
    //
    // For POST the request body is sent synchronously in the constructor (small
    // payloads like JSON logs), then the response is read cooperatively.

    // Completion callback: success, accumulated body (GET/POST response), userData
    using ChunkedCallback = void(*)(bool success, const String& body, void* userData);

    struct ChunkedRequest {
        HttpClient*      http = nullptr;
        String           accum;
        ChunkedCallback  cb = nullptr;
        void*            userData = nullptr;
        bool             _done = false;
        bool             _ok   = false;

        ~ChunkedRequest() { if (http) { http->end(); delete http; } }
        bool done() const { return _done; }
        bool ok()   const { return _ok; }

        // Call once per loop() iteration.  Reads one chunk (~1 KB) and returns.
        // When the response is fully received, fires the callback and sets done().
        void tick() {
            if (_done || !http) return;

            uint8_t buf[1024];
            int n = http->readChunk(buf, sizeof(buf));

            if (n > 0) {
                accum.concat((const char*)buf, n);
                return;
            }

            if (n < 0 || http->bodyDone()) {
                _ok = (n >= 0 || http->bodyDone()) && accum.length() > 0;
                _done = true;
                if (cb) cb(_ok, accum, userData);
                http->end();
                delete http;
                http = nullptr;
                accum = String();  // free memory
            }
            // n == 0: nothing available yet, try next tick
        }
    };

    // Start a cooperative GET.  Returns nullptr on connection failure.
    static ChunkedRequest* chunkedGet(
            const char* url,
            ChunkedCallback cb = nullptr, void* userData = nullptr,
            int timeoutMs = HTTP_TIMEOUT_CATALOG_MS)
    {
        auto* h = new HttpClient(timeoutMs);
        if (!h->get(url)) {
            delete h;
            if (cb) cb(false, String(), userData);
            return nullptr;
        }
        h->beginChunkedRead();

        auto* req = new ChunkedRequest();
        req->http = h;
        req->cb = cb;
        req->userData = userData;
        req->accum.reserve(h->getSize() > 0 ? h->getSize() : 1024);
        return req;
    }

    // Start a cooperative POST.  The request body is sent synchronously (small
    // payloads), then the response is read cooperatively via tick().
    // Returns nullptr on connection failure.
    static ChunkedRequest* chunkedPost(
            const char* url, const String& body,
            ChunkedCallback cb = nullptr, void* userData = nullptr,
            const char* contentType = "application/json",
            int timeoutMs = HTTP_TIMEOUT_SHORT_MS)
    {
        auto* h = new HttpClient(timeoutMs);
        if (!h->post(url, body, contentType)) {
            delete h;
            if (cb) cb(false, String(), userData);
            return nullptr;
        }
        h->beginChunkedRead();

        auto* req = new ChunkedRequest();
        req->http = h;
        req->cb = cb;
        req->userData = userData;
        return req;
    }

    // Process-wide shared WiFiClientSecure — allocated once, never freed.
    // Only safe from the main task (core 1).  Background tasks must call
    // useOwnSecure() instead to get a per-instance client.
    static WiFiClientSecure& sharedSecure() {
        static WiFiClientSecure* s = nullptr;
        if (!s) {
            // Redirect mbedtls allocations to PSRAM — internal RAM is too
            // fragmented for the ~40 KB SSL needs (max_block ≈ 34 KB).
            mbedtls_platform_set_calloc_free(
                [](size_t n, size_t sz) -> void* {
                    void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_8BIT);
                },
                [](void* p) { heap_caps_free(p); }
            );
            s = new WiFiClientSecure();
            s->setInsecure();
        }
        return *s;
    }

private:
    HTTPClient _http;
    int _statusCode;
    int _timeoutMs;
    int _bodyRemaining = -1;  // for chunked reads: bytes left (-1 = unknown/chunked)
    String _statusMsg;

    WiFiClientSecure* _ownSecure = nullptr;  // non-null when running off the main task

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

    // Begin a URL (HTTPS via shared WiFiClientSecure, HTTP via plain begin)
    bool beginUrl(const char* url) {
        _http.setTimeout(_timeoutMs);
        if (strncmp(url, "https", 5) == 0) {
            size_t freeHeap = ESP.getFreeHeap();
            size_t maxBlock = ESP.getMaxAllocHeap();
            if (maxBlock < 40000) {
                Logger.printf("⚠️ HTTPS low heap: free=%u max_block=%u\n",
                              (unsigned)freeHeap, (unsigned)maxBlock);
            }
            return _http.begin(_ownSecure ? *_ownSecure : sharedSecure(), url);
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

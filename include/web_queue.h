#pragma once
/**
 * @file web_queue.h
 * @brief Cooperative chunked web request queue — runs entirely on core 1
 *
 * WebQueue processes HTTP GETs and POSTs one at a time using a cooperative
 * state machine.  Each call to tick() does at most one chunk of I/O
 * (~4 KB, ~2-4 ms) so the main loop can service audio playback, hook-switch
 * polling, and DTMF dispatch between chunks.
 *
 * There is NO FreeRTOS task — all work happens in the caller's context
 * (core 1), which eliminates SD card contention, registry races, and
 * Goertzel starvation.
 *
 * Three item types:
 *   FILE_DL    — GET a URL → write body to an SD card path (audio files)
 *   CATALOG_DL — GET a URL → accumulate body into a String → callback
 *   POST       — POST a body to a URL → callback with response status
 *
 * Usage:
 *   // in loop():
 *   webQueue.tick();   // ~0-4 ms per call
 */

#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "logging.h"

// Forward declaration
class HttpClient;

#ifndef MAX_WEB_QUEUE
#define MAX_WEB_QUEUE 8
#endif
#ifndef WEB_QUEUE_IDLE_INTERVAL_MS
#define WEB_QUEUE_IDLE_INTERVAL_MS 1000
#endif
#ifndef WEB_QUEUE_CHUNK_SIZE
#define WEB_QUEUE_CHUNK_SIZE 4096
#endif

class WebQueue {
public:
    // Completion callback for FILE_DL items.
    // bytesWritten > 0 on success, <= 0 on failure.
    // detectedExt may differ from the enqueued ext (Content-Type correction).
    using FileCallback = void(*)(
        const char* audioKey,
        const char* localPath,
        const char* detectedExt,
        int         bytesWritten,
        void*       userData
    );

    // Completion callback for CATALOG_DL items.
    // Called with the full accumulated response body on success.
    using CatalogCallback = void(*)(bool success, const String& body, void* userData);

    // Completion callback for POST items.
    // Called with success flag and HTTP status code.
    using PostCallback = void(*)(bool success, int statusCode, void* userData);

    enum class EnqueueResult { OK, ALREADY_QUEUED, QUEUE_FULL, BAD_INPUT };

    WebQueue();
    ~WebQueue();

    // -- configuration (call before first tick) ------------------------------
    void setFileCallback(FileCallback cb, void* userData = nullptr);
    void setRegistryMutex(SemaphoreHandle_t mutex);

    // -- queue operations ----------------------------------------------------

    // Enqueue a file download (audio file → SD card).
    EnqueueResult enqueueFile(const char* audioKey,
                              const char* url,
                              const char* localPath,
                              const char* ext = nullptr);

    // Enqueue a catalog download (URL → String → callback).
    // Catalog items are processed before file items.
    EnqueueResult enqueueCatalog(const char* url,
                                 CatalogCallback cb,
                                 void* userData = nullptr);

    // Enqueue an HTTP POST.
    // POST items have the same priority as catalogs (before file downloads).
    // extraHeaderName/Value: one optional custom header (e.g. X-Device-ID).
    EnqueueResult enqueuePost(const char* url,
                              const String& body,
                              PostCallback cb,
                              void* userData = nullptr,
                              const char* contentType = "application/json",
                              const char* extraHeaderName = nullptr,
                              const char* extraHeaderValue = nullptr);

    // Backwards-compatible alias for enqueueFile.
    EnqueueResult enqueue(const char* audioKey,
                          const char* url,
                          const char* localPath,
                          const char* ext = nullptr) {
        return enqueueFile(audioKey, url, localPath, ext);
    }

    void clear();   // cancel all PENDING items
    void reset();   // cancel everything including active request
    void compact(); // reclaim DONE/FAILED/EMPTY slots, shift PENDING to front

    // -- status --------------------------------------------------------------
    int  pendingCount()  const;
    int  totalCount()    const;
    bool isEmpty()       const;
    bool isActive()      const { return _activeIdx >= 0; }
    void listItems()     const;

    // -- cooperative tick — call every loop() iteration ----------------------
    // When idle: rate-limited to WEB_QUEUE_IDLE_INTERVAL_MS.
    // When streaming: reads one chunk (~4 KB) and returns immediately.
    bool tick();

private:
    enum class ItemType  : uint8_t { FILE_DL, CATALOG_DL, POST };
    enum class ItemState : uint8_t { EMPTY, PENDING, IN_PROGRESS, DONE, FAILED };

    struct Item {
        char            audioKey[64];
        char            url[256];
        char            localPath[128];
        char            ext[8];
        ItemType        type;
        ItemState       state;
        // CATALOG_DL callback
        CatalogCallback catalogCb;
        void*           catalogUserData;
        // POST fields
        String          postBody;
        PostCallback    postCb;
        void*           postUserData;
        char            postContentType[48];
        char            postExtraHdrName[32];
        char            postExtraHdrValue[64];
    };

    // -- queue ---------------------------------------------------------------
    Item              _items[MAX_WEB_QUEUE];
    int               _count     = 0;
    SemaphoreHandle_t _regMutex  = nullptr;

    FileCallback      _fileCb         = nullptr;
    void*             _fileCbUserData = nullptr;

    // -- active request state (persists across tick() calls) -----------------
    HttpClient*       _http       = nullptr;   // heap-allocated, owned
    File              _sdFile;                  // open SD file handle (.tmp path during download)
    char              _tmpPath[132];            // .tmp download path (FILE_DL only)
    int               _activeIdx  = -1;        // slot in _items[], or -1
    int               _totalBytes = 0;
    uint8_t           _headerBuf[12];          // first 12 bytes for magic detection
    int               _headerLen  = 0;
    String            _bodyAccum;              // accumulated body (CATALOG_DL / POST response)

    // -- backoff -------------------------------------------------------------
    int               _consecutiveFailures = 0;
    unsigned long     _backoffUntil        = 0;
    unsigned long     _lastIdleTick        = 0;

    // -- internal helpers ----------------------------------------------------
    void  _compact();
    bool  _startNext();
    bool  _streamChunk();
    void  _finishCurrent(bool ok);
    void  _failCurrent();
    void  _cleanupActive();
    Item* _findNextPending();
};

// Global singleton — defined in web_queue.cpp
extern WebQueue webQueue;

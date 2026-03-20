#pragma once
/**
 * @file download_queue.h
 * @brief Non-blocking audio file download queue
 *
 * DownloadQueue manages downloading remote audio files to SD card one at a
 * time.  It is designed to run on a dedicated FreeRTOS task (core 0) so that
 * neither the main audio loop nor the Goertzel DTMF task is affected.
 *
 * Ownership model
 * ---------------
 * - The queue owns a fixed-size circular page of AudioDownloadItem slots.
 * - When the page is exhausted the owner calls refill() (typically from
 *   audio_file_manager after scanning the key registry).
 * - Each item transitions: PENDING → IN_PROGRESS → DONE (or FAILED).
 *
 * Threading
 * ---------
 * - All public methods are safe to call from any task; internal state is
 *   protected by a FreeRTOS mutex.
 * - Registry writes (keyRegistry.registerKey) after a successful download
 *   must be serialised by the caller via registryMutex().
 *
 * Async task usage
 * ----------------
 *   DownloadQueue q;
 *   q.start();                          // launches FreeRTOS task on core 0
 *   q.enqueue(url, localPath, key, ext);
 *   // ...
 *   q.stop();
 *
 * Polled usage (no task, from main loop)
 * ---------------------------------------
 *   DownloadQueue q;
 *   q.enqueue(...);
 *   q.tick();   // call from audioMaintenanceLoop()
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "config.h"
#include "logging.h"

#ifndef MAX_DOWNLOAD_QUEUE
#define MAX_DOWNLOAD_QUEUE 4
#endif
#ifndef DOWNLOAD_QUEUE_CHECK_INTERVAL_MS
#define DOWNLOAD_QUEUE_CHECK_INTERVAL_MS 1000
#endif

// ============================================================================
// DownloadQueue
// ============================================================================

class DownloadQueue {
public:
    // ------------------------------------------------------------------
    // Callback invoked on the download task after each completed/failed
    // item.  bytes > 0 means success; bytes <= 0 means failure.
    // detectedExt may differ from the enqueued ext when Content-Type
    // revealed the real format.
    // ------------------------------------------------------------------
    using CompletionCallback = void(*)(
        const char* audioKey,
        const char* localPath,
        const char* detectedExt,
        int         bytesWritten,
        void*       userData
    );

    // Enqueue result codes
    enum class EnqueueResult { OK, ALREADY_QUEUED, QUEUE_FULL, BAD_PATH };

    DownloadQueue();
    ~DownloadQueue();

    // ------------------------------------------------------------------
    // Configuration — call before start() / first tick()
    // ------------------------------------------------------------------

    // Register a callback invoked after every item completes or fails.
    void setCompletionCallback(CompletionCallback cb, void* userData = nullptr);

    // Optional registry mutex: if set, registerKey() calls are wrapped
    // in take/give around this semaphore.
    void setRegistryMutex(SemaphoreHandle_t mutex);

    // ------------------------------------------------------------------
    // Queue operations (thread-safe)
    // ------------------------------------------------------------------

    // Add a URL to download.
    // audioKey  : registry key (used for logging and callback)
    // url       : remote URL (http/https)
    // localPath : destination on SD card (e.g. "/audio/foo.wav")
    // ext       : hint for file extension; may be corrected by Content-Type
    EnqueueResult enqueue(const char* audioKey,
                          const char* url,
                          const char* localPath,
                          const char* ext = nullptr);

    // Remove all pending (not yet started) items.
    void clear();

    // Reset the whole queue (including in-progress counter) — use after
    // a catalog refresh when local paths may have changed.
    void reset();

    // ------------------------------------------------------------------
    // Status (thread-safe)
    // ------------------------------------------------------------------
    int  pendingCount()  const;   // items not yet started
    int  totalCount()    const;   // total items currently in the page
    bool isEmpty()       const;   // true when no pending items remain
    void listItems()     const;   // log all items to Logger

    // ------------------------------------------------------------------
    // Polled mode — call from audioMaintenanceLoop() when not using task
    // ------------------------------------------------------------------
    // Returns true if a download was started/completed this call.
    // Rate-limited to DOWNLOAD_QUEUE_CHECK_INTERVAL_MS internally.
    bool tick();

    // ------------------------------------------------------------------
    // Async task mode
    // ------------------------------------------------------------------
    // Launches a FreeRTOS task on the given core.
    // priority should be LOWER than the Goertzel task (typically 1).
    void start(UBaseType_t priority = 1, BaseType_t core = 0);
    void stop();                       // signals task to exit and waits
    bool isRunning() const;

private:
    // ------------------------------------------------------------------
    // Internal item state
    // ------------------------------------------------------------------
    enum class ItemState : uint8_t { EMPTY, PENDING, IN_PROGRESS, DONE, FAILED };

    struct Item {
        char      audioKey[64];
        char      url[256];
        char      localPath[128];
        char      ext[8];
        ItemState state;
    };

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    Item              _items[MAX_DOWNLOAD_QUEUE];
    int               _count      = 0;   // number of valid slots (head of array)
    int               _nextIndex  = 0;   // first PENDING slot to process
    SemaphoreHandle_t _mutex      = nullptr;
    SemaphoreHandle_t _regMutex   = nullptr;

    CompletionCallback _cb         = nullptr;
    void*              _cbUserData = nullptr;

    TaskHandle_t       _taskHandle     = nullptr;
    volatile bool      _taskShouldRun  = false;

    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------
    bool        _processNext();           // download one item; returns true on success
    static void _taskEntry(void* param); // FreeRTOS task entry point

    inline bool _lock()   const { return xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE; }
    inline void _unlock() const { xSemaphoreGive(_mutex); }
};

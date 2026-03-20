/**
 * @file download_queue.cpp
 * @brief Non-blocking audio file download queue implementation
 */

#include "download_queue.h"
#include "http_utils.h"
#include "file_utils.h"
#include "audio_key_registry.h"
#include "config.h"
#include <SD.h>
#include <SD_MMC.h>

// SD abstraction (mirrors audio_file_manager.cpp)
#if SD_USE_MMC
  #define DQ_SD_EXISTS(p)      SD_MMC.exists(p)
  #define DQ_SD_OPEN(p, m)     SD_MMC.open(p, m)
  #define DQ_SD_REMOVE(p)      SD_MMC.remove(p)
  #define DQ_SD_RENAME(a, b)   SD_MMC.rename(a, b)
#else
  #define DQ_SD_EXISTS(p)      SD.exists(p)
  #define DQ_SD_OPEN(p, m)     SD.open(p, m)
  #define DQ_SD_REMOVE(p)      SD.remove(p)
  #define DQ_SD_RENAME(a, b)   SD.rename(a, b)
#endif

// ============================================================================
// Forward-declared helper — defined in audio_file_manager.cpp
// ============================================================================
extern AudioKeyRegistry& getAudioKeyRegistry();

// ============================================================================
// MIME → extension table (shared with audio_file_manager.cpp via extern)
// Declare the lookup so this TU can use it without duplicating the table.
// ============================================================================
struct MimeExtMapping { const char* mime; const char* ext; };
extern const MimeExtMapping kMimeExtTable[];
extern const int             kMimeExtTableSize;

static const char* mimeToExt(const char* contentType) {
    if (!contentType || !contentType[0]) return nullptr;
    for (int i = 0; i < kMimeExtTableSize; i++) {
        if (strncmp(contentType, kMimeExtTable[i].mime,
                    strlen(kMimeExtTable[i].mime)) == 0)
            return kMimeExtTable[i].ext;
    }
    return nullptr;
}

// ============================================================================
// Magic-byte format detection (same heuristic as audio_file_manager.cpp)
// ============================================================================
static const char* detectExtFromBytes(const uint8_t* hdr, size_t len) {
    if (len < 12) return nullptr;
    if (memcmp(hdr + 4, "ftyp", 4) == 0)                                     return "m4a";
    if (memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0)      return "wav";
    if (memcmp(hdr, "ID3",  3) == 0 || (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)) return "mp3";
    if (memcmp(hdr, "OggS", 4) == 0)                                           return "ogg";
    if (memcmp(hdr, "fLaC", 4) == 0)                                           return "flac";
    return nullptr;
}

// ============================================================================
// DownloadQueue — construction / destruction
// ============================================================================

DownloadQueue::DownloadQueue() {
    _mutex = xSemaphoreCreateMutex();
    memset(_items, 0, sizeof(_items));
}

DownloadQueue::~DownloadQueue() {
    stop();
    if (_mutex) vSemaphoreDelete(_mutex);
}

// ============================================================================
// Configuration
// ============================================================================

void DownloadQueue::setCompletionCallback(CompletionCallback cb, void* userData) {
    _cb         = cb;
    _cbUserData = userData;
}

void DownloadQueue::setRegistryMutex(SemaphoreHandle_t mutex) {
    _regMutex = mutex;
}

// ============================================================================
// Queue operations
// ============================================================================

DownloadQueue::EnqueueResult DownloadQueue::enqueue(
        const char* audioKey, const char* url,
        const char* localPath, const char* ext)
{
    if (!url || !url[0] || !localPath || !localPath[0])
        return EnqueueResult::BAD_PATH;

    if (!_lock()) return EnqueueResult::QUEUE_FULL; // mutex timeout

    // Duplicate check
    for (int i = 0; i < _count; i++) {
        if (_items[i].state != ItemState::EMPTY &&
            strcmp(_items[i].url, url) == 0) {
            _unlock();
            Logger.printf("ℹ️ [DL] Already queued: %s\n", audioKey ? audioKey : url);
            return EnqueueResult::ALREADY_QUEUED;
        }
    }

    if (_count >= MAX_DOWNLOAD_QUEUE) {
        _unlock();
        Logger.println("⚠️ [DL] Queue full");
        return EnqueueResult::QUEUE_FULL;
    }

    Item& it = _items[_count];
    strncpy(it.audioKey,  audioKey  ? audioKey  : "", sizeof(it.audioKey)  - 1);
    strncpy(it.url,       url,                          sizeof(it.url)       - 1);
    strncpy(it.localPath, localPath,                    sizeof(it.localPath) - 1);
    strncpy(it.ext,       ext       ? ext       : "", sizeof(it.ext)       - 1);
    it.audioKey[sizeof(it.audioKey)   - 1] = '\0';
    it.url[sizeof(it.url)             - 1] = '\0';
    it.localPath[sizeof(it.localPath) - 1] = '\0';
    it.ext[sizeof(it.ext)             - 1] = '\0';
    it.state = ItemState::PENDING;
    _count++;

    Logger.printf("📥 [DL] Queued: %s → %s\n", it.audioKey, it.localPath);
    _unlock();
    return EnqueueResult::OK;
}

void DownloadQueue::clear() {
    if (!_lock()) return;
    for (int i = _nextIndex; i < _count; i++) {
        if (_items[i].state == ItemState::PENDING)
            _items[i].state = ItemState::EMPTY;
    }
    _unlock();
}

void DownloadQueue::reset() {
    if (!_lock()) return;
    memset(_items, 0, sizeof(_items));
    _count     = 0;
    _nextIndex = 0;
    _unlock();
}

// ============================================================================
// Status
// ============================================================================

int DownloadQueue::pendingCount() const {
    if (!_lock()) return 0;
    int n = 0;
    for (int i = _nextIndex; i < _count; i++)
        if (_items[i].state == ItemState::PENDING) n++;
    _unlock();
    return n;
}

int DownloadQueue::totalCount() const {
    if (!_lock()) return 0;
    int n = _count;
    _unlock();
    return n;
}

bool DownloadQueue::isEmpty() const {
    return pendingCount() == 0;
}

void DownloadQueue::listItems() const {
    if (!_lock()) return;
    Logger.printf("📥 DownloadQueue (%d items, next=%d):\n", _count, _nextIndex);
    for (int i = 0; i < _count; i++) {
        const Item& it = _items[i];
        const char* st =
            it.state == ItemState::PENDING     ? "⏳ pending"     :
            it.state == ItemState::IN_PROGRESS ? "🔄 in-progress" :
            it.state == ItemState::DONE        ? "✅ done"        :
            it.state == ItemState::FAILED      ? "❌ failed"      : "   empty";
        Logger.printf("  [%d] %s  %s → %s\n", i, st, it.audioKey, it.localPath);
    }
    _unlock();
}

// ============================================================================
// Polled tick
// ============================================================================

bool DownloadQueue::tick() {
    static unsigned long lastTick = 0;
    unsigned long now = millis();
    if (now - lastTick < DOWNLOAD_QUEUE_CHECK_INTERVAL_MS) return false;
    lastTick = now;
    return _processNext();
}

// ============================================================================
// Async task
// ============================================================================

void DownloadQueue::start(UBaseType_t priority, BaseType_t core) {
    if (_taskHandle) {
        Logger.println("⚠️ [DL] Task already running");
        return;
    }
    _taskShouldRun = true;
    xTaskCreatePinnedToCore(
        _taskEntry, "AudioDL", 12288, this,
        priority, &_taskHandle, core
    );
    Logger.printf("📥 [DL] Task started (core %d, pri %d)\n", (int)core, (int)priority);
}

void DownloadQueue::stop() {
    if (!_taskHandle) return;
    _taskShouldRun = false;
    for (int i = 0; i < 200 && _taskHandle; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (_taskHandle) {
        vTaskDelete(_taskHandle);
        _taskHandle = nullptr;
    }
}

bool DownloadQueue::isRunning() const {
    return _taskHandle != nullptr && _taskShouldRun;
}

void DownloadQueue::_taskEntry(void* param) {
    DownloadQueue* q = static_cast<DownloadQueue*>(param);
    Logger.println("📥 [DL] Task loop started");
    while (q->_taskShouldRun) {
        if (!q->_processNext()) {
            // Nothing to do — sleep longer to yield to Goertzel task
            vTaskDelay(pdMS_TO_TICKS(DOWNLOAD_QUEUE_CHECK_INTERVAL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // brief yield after a download
        }
    }
    Logger.println("📥 [DL] Task stopped");
    q->_taskHandle = nullptr;
    vTaskDelete(nullptr);
}

// ============================================================================
// Core download logic
// ============================================================================

bool DownloadQueue::_processNext() {
    static int  consecutiveFailures = 0;
    static unsigned long backoffUntil = 0;

    if (consecutiveFailures > 0 && millis() < backoffUntil)
        return false;

    // --- pick next pending item under lock ---
    if (!_lock()) return false;
    Item* item = nullptr;
    for (int i = _nextIndex; i < _count; i++) {
        if (_items[i].state == ItemState::PENDING) {
            item = &_items[i];
            item->state = ItemState::IN_PROGRESS;
            _nextIndex = i + 1;
            break;
        }
    }
    _unlock();

    if (!item) return false; // nothing pending

    Logger.printf("📥 [DL] %s\n     %s\n  → %s\n",
                  item->audioKey, item->url, item->localPath);

    // --- HTTP download ---
    HttpClient http(HTTP_TIMEOUT_DOWNLOAD_MS);
    http.useOwnSecure(); // must not touch shared singleton from a background task
    const char* wantHeaders[] = {"Content-Type"};
    http.collectHeaders(wantHeaders, 1);

    if (!http.get(item->url)) {
        Logger.printf("❌ [DL] HTTP %d for %s\n", http.statusCode(), item->audioKey);
        if (!_lock()) return false;
        item->state = ItemState::FAILED;
        _unlock();

        consecutiveFailures++;
        unsigned long backoff = min(300000UL, 10000UL << min(consecutiveFailures - 1, 5));
        backoffUntil = millis() + backoff;
        Logger.printf("⏳ [DL] Backoff %lus after %d failure(s)\n",
                      backoff / 1000, consecutiveFailures);
        if (_cb) _cb(item->audioKey, item->localPath, item->ext, -1, _cbUserData);
        return false;
    }

    // --- Content-Type → corrected extension ---
    String ct = http.header("Content-Type");
    const char* detectedExt = mimeToExt(ct.c_str());
    if (detectedExt && item->ext[0] && strcmp(detectedExt, item->ext) != 0) {
        Logger.printf("🔍 [DL] Content-Type '%s' → '%s' (was '%s')\n",
                      ct.c_str(), detectedExt, item->ext);
    }
    if (detectedExt) {
        // Recompute localPath with corrected extension
        char corrected[128];
        if (getLocalPathForUrl(item->url, corrected, detectedExt)) {
            if (!_lock()) { http.end(); return false; }
            strncpy(item->localPath, corrected, sizeof(item->localPath) - 1);
            item->localPath[sizeof(item->localPath) - 1] = '\0';
            strncpy(item->ext, detectedExt, sizeof(item->ext) - 1);
            item->ext[sizeof(item->ext) - 1] = '\0';
            _unlock();
        }
    }

    // --- Remove stale files with wrong extension for the same base name ---
    char base[64]; urlToBaseFilename(item->url, base, nullptr);
    if (char* dot = strrchr(base, '.')) *dot = '\0';
    const char* allExts[] = {".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a"};
    for (int i = 0; i < 6; i++) {
        char old[128];
        snprintf(old, sizeof(old), "%s/%s%s", AUDIO_FILES_DIR, base, allExts[i]);
        if (strcmp(old, item->localPath) != 0 && DQ_SD_EXISTS(old)) {
            Logger.printf("🗑️ [DL] Remove stale: %s\n", old);
            DQ_SD_REMOVE(old);
        }
    }

    // --- Write to SD ---
    File f = DQ_SD_OPEN(item->localPath, FILE_WRITE);
    if (!f) {
        Logger.printf("❌ [DL] Cannot create file: %s\n", item->localPath);
        http.end();
        if (!_lock()) return false;
        item->state = ItemState::FAILED;
        _unlock();
        if (_cb) _cb(item->audioKey, item->localPath, item->ext, -1, _cbUserData);
        return false;
    }

    int totalBytes = http.streamBody([&](const uint8_t* buf, size_t len) {
        f.write(buf, len);
        return true;
    });
    f.close();

    if (totalBytes <= 0) {
        Logger.printf("❌ [DL] Zero bytes written for %s\n", item->audioKey);
        if (!_lock()) return false;
        item->state = ItemState::FAILED;
        _unlock();
        consecutiveFailures++;
        unsigned long backoff = min(300000UL, 10000UL << min(consecutiveFailures - 1, 5));
        backoffUntil = millis() + backoff;
        if (_cb) _cb(item->audioKey, item->localPath, item->ext, -1, _cbUserData);
        return false;
    }

    Logger.printf("✅ [DL] %d bytes → %s\n", totalBytes, item->localPath);

    // --- Magic-byte validation: Google Drive may lie about Content-Type ---
    File verify = DQ_SD_OPEN(item->localPath, FILE_READ);
    if (verify && verify.size() >= 12) {
        uint8_t hdr[12];
        verify.read(hdr, 12);
        verify.close();

        const char* actualExt = detectExtFromBytes(hdr, 12);
        if (actualExt && item->ext[0] && strcmp(actualExt, item->ext) != 0) {
            Logger.printf("⚠️ [DL] '%s' content is %s not %s — renaming\n",
                          item->audioKey, actualExt, item->ext);
            char newPath[128];
            if (getLocalPathForUrl(item->url, newPath, actualExt)) {
                DQ_SD_RENAME(item->localPath, newPath);
                if (!_lock()) return false;
                strncpy(item->localPath, newPath, sizeof(item->localPath) - 1);
                item->localPath[sizeof(item->localPath) - 1] = '\0';
                strncpy(item->ext, actualExt, sizeof(item->ext) - 1);
                item->ext[sizeof(item->ext) - 1] = '\0';
                _unlock();
            }
        }
    } else if (verify) {
        verify.close();
    }

    consecutiveFailures = 0;

    // --- Re-register key with correct extension (caller-supplied mutex) ---
    AudioKeyRegistry& reg = getAudioKeyRegistry();
    if (_regMutex) xSemaphoreTake(_regMutex, portMAX_DELAY);
    reg.registerKey(item->audioKey, item->url, item->ext[0] ? item->ext : nullptr);
    if (_regMutex) xSemaphoreGive(_regMutex);

    if (!_lock()) return false;
    item->state = ItemState::DONE;
    _unlock();

    if (_cb) _cb(item->audioKey, item->localPath, item->ext, totalBytes, _cbUserData);
    return true;
}

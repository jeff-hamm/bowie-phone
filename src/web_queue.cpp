/**
 * @file web_queue.cpp
 * @brief Cooperative chunked web request queue — state machine implementation
 *
 * All work runs on core 1 via tick().  Each tick() call does at most one
 * ~4 KB chunk of I/O so the main loop can service audio playback between
 * chunks.  See web_queue.h for the full design.
 */

#include "web_queue.h"
#include "http_utils.h"
#include "file_utils.h"
#include "audio_key_registry.h"
#include "config.h"
#include <SD.h>
#include <SD_MMC.h>

// Global singleton
WebQueue webQueue;

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
// Magic-byte format detection
// ============================================================================
static const char* detectExtFromBytes(const uint8_t* hdr, size_t len) {
    if (len < 12) return nullptr;
    if (memcmp(hdr + 4, "ftyp", 4) == 0)                                      return "m4a";
    if (memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0)       return "wav";
    if (memcmp(hdr, "ID3",  3) == 0 || (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)) return "mp3";
    if (memcmp(hdr, "OggS", 4) == 0)                                           return "ogg";
    if (memcmp(hdr, "fLaC", 4) == 0)                                           return "flac";
    return nullptr;
}

// ============================================================================
// Construction / destruction
// ============================================================================

WebQueue::WebQueue() {
    memset(_items, 0, sizeof(_items));
    memset(_headerBuf, 0, sizeof(_headerBuf));
    memset(_tmpPath, 0, sizeof(_tmpPath));
}

WebQueue::~WebQueue() {
    _cleanupActive();
}

// ============================================================================
// Configuration
// ============================================================================

void WebQueue::setFileCallback(FileCallback cb, void* userData) {
    _fileCb         = cb;
    _fileCbUserData = userData;
}

void WebQueue::setRegistryMutex(SemaphoreHandle_t mutex) {
    _regMutex = mutex;
}

// ============================================================================
// Queue operations
// ============================================================================

WebQueue::EnqueueResult WebQueue::enqueueFile(
        const char* audioKey, const char* url,
        const char* localPath, const char* ext)
{
    if (!url || !url[0] || !localPath || !localPath[0])
        return EnqueueResult::BAD_INPUT;

    // Duplicate check
    for (int i = 0; i < _count; i++) {
        if (_items[i].state != ItemState::EMPTY &&
            strcmp(_items[i].url, url) == 0) {
            return EnqueueResult::ALREADY_QUEUED;
        }
    }

    if (_count >= MAX_WEB_QUEUE)
        return EnqueueResult::QUEUE_FULL;

    Item& it = _items[_count];
    memset(&it, 0, sizeof(it));
    strncpy(it.audioKey,  audioKey  ? audioKey  : "", sizeof(it.audioKey)  - 1);
    strncpy(it.url,       url,                         sizeof(it.url)       - 1);
    strncpy(it.localPath, localPath,                   sizeof(it.localPath) - 1);
    strncpy(it.ext,       ext       ? ext       : "", sizeof(it.ext)       - 1);
    it.type  = ItemType::FILE_DL;
    it.state = ItemState::PENDING;
    it.catalogCb       = nullptr;
    it.catalogUserData = nullptr;
    _count++;

    Logger.printf("📥 [WQ] Queued file: %s → %s\n", it.audioKey, it.localPath);
    return EnqueueResult::OK;
}

WebQueue::EnqueueResult WebQueue::enqueueCatalog(
        const char* url, CatalogCallback cb, void* userData)
{
    if (!url || !url[0] || !cb)
        return EnqueueResult::BAD_INPUT;

    // Duplicate check
    for (int i = 0; i < _count; i++) {
        if (_items[i].state != ItemState::EMPTY &&
            _items[i].type == ItemType::CATALOG_DL &&
            strcmp(_items[i].url, url) == 0) {
            return EnqueueResult::ALREADY_QUEUED;
        }
    }

    if (_count >= MAX_WEB_QUEUE)
        return EnqueueResult::QUEUE_FULL;

    Item& it = _items[_count];
    memset(&it, 0, sizeof(it));
    strncpy(it.url, url, sizeof(it.url) - 1);
    snprintf(it.audioKey, sizeof(it.audioKey), "(catalog)");
    it.type            = ItemType::CATALOG_DL;
    it.state           = ItemState::PENDING;
    it.catalogCb       = cb;
    it.catalogUserData = userData;
    _count++;

    Logger.printf("📥 [WQ] Queued catalog: %s\n", url);
    return EnqueueResult::OK;
}

WebQueue::EnqueueResult WebQueue::enqueuePost(
        const char* url, const String& body,
        PostCallback cb, void* userData,
        const char* contentType,
        const char* extraHeaderName, const char* extraHeaderValue)
{
    if (!url || !url[0] || !cb)
        return EnqueueResult::BAD_INPUT;

    if (_count >= MAX_WEB_QUEUE)
        return EnqueueResult::QUEUE_FULL;

    Item& it = _items[_count];
    memset(&it, 0, sizeof(it));
    strncpy(it.url, url, sizeof(it.url) - 1);
    snprintf(it.audioKey, sizeof(it.audioKey), "(post)");
    it.type          = ItemType::POST;
    it.state         = ItemState::PENDING;
    it.postBody      = body;
    it.postCb        = cb;
    it.postUserData  = userData;
    if (contentType)
        strncpy(it.postContentType, contentType, sizeof(it.postContentType) - 1);
    if (extraHeaderName)
        strncpy(it.postExtraHdrName, extraHeaderName, sizeof(it.postExtraHdrName) - 1);
    if (extraHeaderValue)
        strncpy(it.postExtraHdrValue, extraHeaderValue, sizeof(it.postExtraHdrValue) - 1);
    _count++;

    Logger.printf("📤 [WQ] Queued POST: %s (%d bytes)\n", url, body.length());
    return EnqueueResult::OK;
}

void WebQueue::clear() {
    for (int i = 0; i < _count; i++) {
        if (_items[i].state == ItemState::PENDING) {
            _items[i].postBody = String();  // free heap before marking empty
            _items[i].state = ItemState::EMPTY;
        }
    }
}

void WebQueue::reset() {
    _cleanupActive();
    for (int i = 0; i < _count; i++)
        _items[i].postBody = String();  // free heap before zeroing
    memset(_items, 0, sizeof(_items));
    _count     = 0;
    _consecutiveFailures = 0;
    _backoffUntil        = 0;
}

void WebQueue::compact() {
    _compact();
}

void WebQueue::_compact() {
    if (_activeIdx >= 0) return;  // don't compact while a download is active
    int dst = 0;
    for (int src = 0; src < _count; src++) {
        if (_items[src].state == ItemState::PENDING) {
            if (dst != src)
                _items[dst] = _items[src];
            dst++;
        }
    }
    if (dst < _count) {
        // Properly destruct String members before zeroing to avoid heap leaks
        for (int i = dst; i < _count; i++) {
            _items[i].postBody = String();
            memset(&_items[i], 0, sizeof(Item));
        }
        _count = dst;
    }
}

// ============================================================================
// Status
// ============================================================================

int WebQueue::pendingCount() const {
    int n = 0;
    for (int i = 0; i < _count; i++)
        if (_items[i].state == ItemState::PENDING) n++;
    return n;
}

int WebQueue::totalCount() const {
    return _count;
}

bool WebQueue::isEmpty() const {
    return pendingCount() == 0 && _activeIdx < 0;
}

void WebQueue::listItems() const {
    Logger.printf("📥 WebQueue (%d items, active=%d):\n",
                  _count, _activeIdx);
    for (int i = 0; i < _count; i++) {
        const Item& it = _items[i];
        const char* st =
            it.state == ItemState::PENDING     ? "⏳ pending"     :
            it.state == ItemState::IN_PROGRESS ? "🔄 streaming"   :
            it.state == ItemState::DONE        ? "✅ done"        :
            it.state == ItemState::FAILED      ? "❌ failed"      : "   empty";
        const char* tp = it.type == ItemType::CATALOG_DL ? "CAT" :
                         it.type == ItemType::POST       ? "POST" : "FILE";
        Logger.printf("  [%d] %s %s  %s → %s\n", i, tp, st, it.audioKey,
                      it.type == ItemType::FILE_DL ? it.localPath : "(string)");
    }
}

// ============================================================================
// Cooperative tick — the heart of the state machine
// ============================================================================

bool WebQueue::tick() {
    // Active request in progress → stream one chunk
    if (_activeIdx >= 0) {
        return _streamChunk();
    }

    // No active request — rate-limit idle checks
    unsigned long now = millis();
    if (now - _lastIdleTick < WEB_QUEUE_IDLE_INTERVAL_MS)
        return false;
    _lastIdleTick = now;

    // Backoff after failures
    if (_consecutiveFailures > 0 && now < _backoffUntil)
        return false;

    // Compact when >=50% of slots are consumed by done/failed/empty items
    int nonPending = _count - pendingCount();
    if (_count > 0 && nonPending >= MAX_WEB_QUEUE / 2)
        _compact();

    // Try to start the next pending item
    return _startNext();
}

// ============================================================================
// _findNextPending — catalog items have priority over file items
// ============================================================================

WebQueue::Item* WebQueue::_findNextPending() {
    // First pass: find first CATALOG or POST PENDING (same priority)
    for (int i = 0; i < _count; i++) {
        if (_items[i].state == ItemState::PENDING &&
            (_items[i].type == ItemType::CATALOG_DL || _items[i].type == ItemType::POST))
            return &_items[i];
    }
    // Second pass: first FILE PENDING
    for (int i = 0; i < _count; i++) {
        if (_items[i].state == ItemState::PENDING &&
            _items[i].type == ItemType::FILE_DL)
            return &_items[i];
    }
    return nullptr;
}

// ============================================================================
// _startNext — open HTTP connection + prepare SD file / String accumulator
// ============================================================================

bool WebQueue::_startNext() {
    Item* item = _findNextPending();
    if (!item) return false;

    int idx = (int)(item - _items);

    const char* label = item->type == ItemType::CATALOG_DL ? "catalog" :
                        item->type == ItemType::POST       ? "POST"    : item->audioKey;
    Logger.printf("📥 [WQ] Starting %s: %s\n", label, item->url);

    // Allocate HTTP client (on heap so it persists across tick calls)
    int timeout = item->type == ItemType::CATALOG_DL ? HTTP_TIMEOUT_CATALOG_MS
                : item->type == ItemType::POST       ? HTTP_TIMEOUT_SHORT_MS
                :                                      HTTP_TIMEOUT_DOWNLOAD_MS;
    _http = new HttpClient(timeout);

    if (item->type == ItemType::POST) {
        // --- POST: send body, then read response via chunked reader ---
        HttpClient::Header hdrs[2];
        int hdrCount = 0;
        if (item->postContentType[0]) {
            hdrs[hdrCount++] = {"Content-Type", item->postContentType};
        }
        if (item->postExtraHdrName[0] && item->postExtraHdrValue[0]) {
            hdrs[hdrCount++] = {item->postExtraHdrName, item->postExtraHdrValue};
        }

        if (!_http->post(item->url, item->postBody, hdrs, hdrCount)) {
            int code = _http->statusCode();
            Logger.printf("❌ [WQ] POST HTTP %d for %s\n", code, item->url);
            // Fire callback with failure
            if (item->postCb) item->postCb(false, code, item->postUserData);
            delete _http; _http = nullptr;
            item->postBody = String(); // free memory
            item->state = ItemState::FAILED;
            _consecutiveFailures++;
            unsigned long backoff = min(300000UL, 10000UL << min(_consecutiveFailures - 1, 5));
            _backoffUntil = millis() + backoff;
            Logger.printf("⏳ [WQ] Backoff %lus after %d failure(s)\n",
                          backoff / 1000, _consecutiveFailures);
            return false;
        }

        // POST succeeded — fire callback immediately
        int code = _http->statusCode();
        _http->end();
        delete _http; _http = nullptr;

        item->postBody = String(); // free memory
        item->state = ItemState::DONE;
        _consecutiveFailures = 0;
        Logger.printf("✅ [WQ] POST %d → %s\n", code, item->url);
        if (item->postCb) item->postCb(true, code, item->postUserData);
        return true;
    }

    // --- GET (FILE_DL or CATALOG_DL) ---
    const char* wantHeaders[] = {"Content-Type"};
    _http->collectHeaders(wantHeaders, 1);

    if (!_http->get(item->url)) {
        Logger.printf("❌ [WQ] HTTP %d for %s\n", _http->statusCode(), item->audioKey);
        delete _http; _http = nullptr;
        item->state = ItemState::FAILED;
        _consecutiveFailures++;
        unsigned long backoff = min(300000UL, 10000UL << min(_consecutiveFailures - 1, 5));
        _backoffUntil = millis() + backoff;
        Logger.printf("⏳ [WQ] Backoff %lus after %d failure(s)\n",
                      backoff / 1000, _consecutiveFailures);

        if (item->type == ItemType::FILE_DL && _fileCb)
            _fileCb(item->audioKey, item->localPath, item->ext, -1, _fileCbUserData);
        if (item->type == ItemType::CATALOG_DL && item->catalogCb)
            item->catalogCb(false, String(), item->catalogUserData);
        return false;
    }

    // Connection established — mark IN_PROGRESS
    item->state = ItemState::IN_PROGRESS;
    _activeIdx  = idx;
    _totalBytes = 0;
    _headerLen  = 0;

    _http->beginChunkedRead();

    if (item->type == ItemType::FILE_DL) {
        // --- Content-Type → corrected extension ---
        String ct = _http->header("Content-Type");
        const char* detectedExt = mimeToExt(ct.c_str());
        if (detectedExt && item->ext[0] && strcmp(detectedExt, item->ext) != 0) {
            Logger.printf("🔍 [WQ] Content-Type '%s' → '%s' (was '%s')\n",
                          ct.c_str(), detectedExt, item->ext);
        }
        if (detectedExt) {
            char corrected[128];
            if (getLocalPathForUrl(item->url, corrected, detectedExt)) {
                strncpy(item->localPath, corrected, sizeof(item->localPath) - 1);
                item->localPath[sizeof(item->localPath) - 1] = '\0';
                strncpy(item->ext, detectedExt, sizeof(item->ext) - 1);
                item->ext[sizeof(item->ext) - 1] = '\0';
            }
        }

        // --- Remove stale files with wrong extension ---
        char base[64]; urlToBaseFilename(item->url, base, nullptr);
        if (char* dot = strrchr(base, '.')) *dot = '\0';
        const char* allExts[] = {".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a"};
        for (int i = 0; i < 6; i++) {
            char old[128];
            snprintf(old, sizeof(old), "%s/%s%s", AUDIO_FILES_DIR, base, allExts[i]);
            if (strcmp(old, item->localPath) != 0 && DQ_SD_EXISTS(old)) {
                Logger.printf("🗑️ [WQ] Remove stale: %s\n", old);
                DQ_SD_REMOVE(old);
            }
        }

        // --- Open SD file for writing to temp path ---
        snprintf(_tmpPath, sizeof(_tmpPath), "%s.tmp", item->localPath);
        _sdFile = DQ_SD_OPEN(_tmpPath, FILE_WRITE);
        if (!_sdFile) {
            Logger.printf("❌ [WQ] Cannot create file: %s\n", _tmpPath);
            _failCurrent();
            return false;
        }
    } else {
        // CATALOG: prepare String accumulator
        _bodyAccum = String();
        _bodyAccum.reserve(_http->getSize() > 0 ? _http->getSize() : 4096);
    }

    return true;
}

// ============================================================================
// _streamChunk — read one chunk (~4 KB) from HTTP → SD file or String
// ============================================================================

bool WebQueue::_streamChunk() {
    if (_activeIdx < 0 || !_http) return false;
    Item& item = _items[_activeIdx];

    uint8_t buf[WEB_QUEUE_CHUNK_SIZE];
    int n = _http->readChunk(buf, sizeof(buf));

    if (n > 0) {
        // Capture first 12 bytes for magic-byte detection (FILE_DL only)
        if (item.type == ItemType::FILE_DL && _headerLen < (int)sizeof(_headerBuf)) {
            int tocopy = min(n, (int)sizeof(_headerBuf) - _headerLen);
            memcpy(_headerBuf + _headerLen, buf, tocopy);
            _headerLen += tocopy;
        }

        if (item.type == ItemType::FILE_DL) {
            _sdFile.write(buf, n);
        } else {
            _bodyAccum.concat((const char*)buf, n);
        }
        _totalBytes += n;
        return true;
    }

    if (n < 0) {
        // TCP error / connection lost — treat as failure even if partial data received
        Logger.printf("❌ [WQ] Read error for %s (%d bytes so far)\n", item.audioKey, _totalBytes);
        _failCurrent();
        return true;
    }

    if (_http->bodyDone()) {
        // Body complete
        bool ok = (_totalBytes > 0);
        if (!ok) {
            Logger.printf("❌ [WQ] Zero bytes for %s\n", item.audioKey);
            _failCurrent();
        } else {
            _finishCurrent(true);
        }
        return true;
    }

    // n == 0 and body not done: nothing available yet, try next tick
    return false;
}

// ============================================================================
// _finishCurrent — close file, magic-byte verify, re-register key, callback
// ============================================================================

void WebQueue::_finishCurrent(bool ok) {
    if (_activeIdx < 0) return;
    Item& item = _items[_activeIdx];

    if (item.type == ItemType::FILE_DL) {
        _sdFile.close();

        if (ok) {
            // Magic-byte validation: Google Drive may lie about Content-Type
            if (_headerLen >= 12) {
                const char* actualExt = detectExtFromBytes(_headerBuf, _headerLen);
                if (actualExt && item.ext[0] && strcmp(actualExt, item.ext) != 0) {
                    Logger.printf("⚠️ [WQ] '%s' content is %s not %s — correcting\n",
                                  item.audioKey, actualExt, item.ext);
                    char newPath[128];
                    if (getLocalPathForUrl(item.url, newPath, actualExt)) {
                        strncpy(item.localPath, newPath, sizeof(item.localPath) - 1);
                        item.localPath[sizeof(item.localPath) - 1] = '\0';
                        strncpy(item.ext, actualExt, sizeof(item.ext) - 1);
                        item.ext[sizeof(item.ext) - 1] = '\0';
                    }
                }
            }

            // Atomic replace: remove any existing file, rename .tmp → final
            if (DQ_SD_EXISTS(item.localPath))
                DQ_SD_REMOVE(item.localPath);
            DQ_SD_RENAME(_tmpPath, item.localPath);
            Logger.printf("✅ [WQ] %d bytes → %s\n", _totalBytes, item.localPath);
        }

        item.state = ok ? ItemState::DONE : ItemState::FAILED;
        if (_fileCb)
            _fileCb(item.audioKey, item.localPath, item.ext,
                    ok ? _totalBytes : -1, _fileCbUserData);
    } else {
        // CATALOG_DL
        Logger.printf("✅ [WQ] Catalog received (%d bytes)\n", _totalBytes);
        item.state = ok ? ItemState::DONE : ItemState::FAILED;
        if (item.catalogCb)
            item.catalogCb(ok, _bodyAccum, item.catalogUserData);
        _bodyAccum = String(); // release memory
    }

    _consecutiveFailures = 0;
    _cleanupActive();
}

// ============================================================================
// _failCurrent — mark failed, apply backoff, fire callback
// ============================================================================

void WebQueue::_failCurrent() {
    if (_activeIdx < 0) return;
    Item& item = _items[_activeIdx];

    if (item.type == ItemType::FILE_DL) {
        _sdFile.close();
        // Clean up .tmp partial file — never touch the real file
        if (_tmpPath[0] && DQ_SD_EXISTS(_tmpPath))
            DQ_SD_REMOVE(_tmpPath);
    }

    item.postBody = String();  // free any POST body heap memory
    item.state = ItemState::FAILED;
    _consecutiveFailures++;
    unsigned long backoff = min(300000UL, 10000UL << min(_consecutiveFailures - 1, 5));
    _backoffUntil = millis() + backoff;
    Logger.printf("⏳ [WQ] Backoff %lus after %d failure(s)\n",
                  backoff / 1000, _consecutiveFailures);

    if (item.type == ItemType::FILE_DL && _fileCb)
        _fileCb(item.audioKey, item.localPath, item.ext, -1, _fileCbUserData);
    if (item.type == ItemType::CATALOG_DL && item.catalogCb)
        item.catalogCb(false, String(), item.catalogUserData);

    _bodyAccum = String();
    _cleanupActive();
}

// ============================================================================
// _cleanupActive — release HTTP client + file handle, reset active index
// ============================================================================

void WebQueue::_cleanupActive() {
    if (_http) {
        _http->end();
        delete _http;
        _http = nullptr;
    }
    if (_sdFile) _sdFile.close();
    _bodyAccum = String();
    _tmpPath[0] = '\0';
    _activeIdx  = -1;
    _totalBytes = 0;
    _headerLen  = 0;
}

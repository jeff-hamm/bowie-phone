#ifdef TEST_MODE

#include "support/sequence_integration_fakes.h"

#include "audio_key_registry.h"
#include "extended_audio_player.h"
#include "notifications.h"
#include "special_command_processor.h"

#include <cstring>

static AudioPlayerSpyState g_audioSpy = {};
static bool g_playPlaylistResult = false;
static AudioKeyRegistry g_registry;

ExtendedAudioPlayer audioPlayer;

AudioPlayerSpyState& getAudioPlayerSpyState() {
    return g_audioSpy;
}

void resetAudioPlayerSpyState() {
    memset(&g_audioSpy, 0, sizeof(g_audioSpy));
}

void setPlayPlaylistResultForTest(bool result) {
    g_playPlaylistResult = result;
}

void clearRegistryForTest() {
    g_registry.clearKeys();
}

void registerMockAudioKeyForTest(const char* key) {
    if (!key || strlen(key) == 0) {
        return;
    }
    g_registry.registerKey(key, "/test/mock.wav", AudioStreamType::FILE_STREAM, nullptr);
}


ExtendedAudioPlayer& getExtendedAudioPlayer() {
    return audioPlayer;
}

ExtendedAudioPlayer::ExtendedAudioPlayer(int urlStreamBufferSize) {
    this->urlStreamBufferSize = urlStreamBufferSize;
    initialized = true;
    currentKey[0] = '\0';
}

ExtendedAudioPlayer::~ExtendedAudioPlayer() {}

bool ExtendedAudioPlayer::playPlaylist(const char* playlistName) {
    g_audioSpy.playPlaylistCalls++;
    g_audioSpy.lastPlaylist[0] = '\0';
    if (playlistName) {
        strncpy(g_audioSpy.lastPlaylist, playlistName, sizeof(g_audioSpy.lastPlaylist) - 1);
        g_audioSpy.lastPlaylist[sizeof(g_audioSpy.lastPlaylist) - 1] = '\0';
    }
    return g_playPlaylistResult;
}

bool ExtendedAudioPlayer::playAudioKey(const char* audioKey, unsigned long durationMs) {
    (void)durationMs;
    g_audioSpy.playAudioKeyCalls++;
    g_audioSpy.lastAudioKey[0] = '\0';
    g_audioSpy.currentKey[0] = '\0';

    if (audioKey) {
        strncpy(g_audioSpy.lastAudioKey, audioKey, sizeof(g_audioSpy.lastAudioKey) - 1);
        g_audioSpy.lastAudioKey[sizeof(g_audioSpy.lastAudioKey) - 1] = '\0';
        strncpy(g_audioSpy.currentKey, audioKey, sizeof(g_audioSpy.currentKey) - 1);
        g_audioSpy.currentKey[sizeof(g_audioSpy.currentKey) - 1] = '\0';
    }

    g_audioSpy.active = true;
    return true;
}

void ExtendedAudioPlayer::stop() {
    g_audioSpy.stopCalls++;
    g_audioSpy.active = false;
    g_audioSpy.currentKey[0] = '\0';
}

bool ExtendedAudioPlayer::isAudioKeyPlaying(const char* audioKey) const {
    if (!audioKey || !g_audioSpy.active) {
        return false;
    }
    return strcmp(g_audioSpy.currentKey, audioKey) == 0;
}

bool ExtendedAudioPlayer::isActive() const {
    return g_audioSpy.active;
}

void initNotifications() {}

void notify(NotificationType type, bool value) {
    (void)type;
    (void)value;
}

void notify(NotificationType type, int value) {
    (void)type;
    (void)value;
}

void setPulseConfig(const PulseConfig& config) {
    (void)config;
}

PulseConfig getPulseConfig() {
    PulseConfig config = {0, 0, 0};
    return config;
}

bool notificationsEnabled() {
    return false;
}

bool isSpecialCommand(const char* sequence) {
    (void)sequence;
    return false;
}

void processSpecialCommand(const char* sequence) {
    (void)sequence;
}

#endif // TEST_MODE

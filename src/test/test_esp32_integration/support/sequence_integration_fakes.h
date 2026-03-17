#pragma once

#ifdef TEST_MODE

#include <Arduino.h>

struct AudioPlayerSpyState {
    bool active;
    int playPlaylistCalls;
    int playAudioKeyCalls;
    int stopCalls;
    char currentKey[32];
    char lastPlaylist[32];
    char lastAudioKey[32];
};

AudioPlayerSpyState& getAudioPlayerSpyState();
void resetAudioPlayerSpyState();
void setPlayPlaylistResultForTest(bool result);

void clearRegistryForTest();
void registerMockAudioKeyForTest(const char* key);

#endif // TEST_MODE

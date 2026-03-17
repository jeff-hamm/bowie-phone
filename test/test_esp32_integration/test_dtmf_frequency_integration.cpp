#include <Arduino.h>
#include <unity.h>

#ifdef TEST_MODE

#include "config.h"
#include "dtmf_goertzel.h"
#include "extended_audio_player.h"
#include "phone.h"
#include "phone_service.h"
#include "sequence_processor.h"
#include "test_helpers/dtmf_goertzel_test_helpers.h"
#include "test_helpers/dtmf_tone_test_helpers.h"
#include "support/sequence_integration_fakes.h"

#include <cstring>
#include <vector>

extern ExtendedAudioPlayer audioPlayer;

namespace {

const int GOERTZEL_RELEASE_BLOCKS_FOR_TEST = 4;

bool mapDigitToRowCol(char digit, int& row, int& col) {
    switch (digit) {
        case '1': row = 0; col = 0; return true;
        case '2': row = 0; col = 1; return true;
        case '3': row = 0; col = 2; return true;
        case '4': row = 1; col = 0; return true;
        case '5': row = 1; col = 1; return true;
        case '6': row = 1; col = 2; return true;
        case '7': row = 2; col = 0; return true;
        case '8': row = 2; col = 1; return true;
        case '9': row = 2; col = 2; return true;
        case '*': row = 3; col = 0; return true;
        case '0': row = 3; col = 1; return true;
        case '#': row = 3; col = 2; return true;
        default: return false;
    }
}

void initGoertzelForTest(GoertzelStream& goertzel) {
    StreamCopy dummyCopier;
    initGoertzelDecoder(goertzel, dummyCopier);
}

void runMainLikeSequenceStep() {
    char detected = getGoertzelKey();
    if (detected != 0) {
        addDtmfDigit(detected);
    }
    readDTMFSequence(true);
}

void feedSilenceBlocks(GoertzelStream& goertzel, int blocks, float& phaseOffset) {
    const PhoneConfig& config = getPhoneConfig();
    const int blockSize = config.goertzelBlockSize;
    std::vector<int16_t> silenceBlock(blockSize);

    generateSilenceBlockForTest(silenceBlock.data(), silenceBlock.size());
    for (int i = 0; i < blocks; i++) {
        processGoertzelSamplesForTest(goertzel, silenceBlock.data(), silenceBlock.size());
        runMainLikeSequenceStep();
        phaseOffset += static_cast<float>(blockSize);
    }
}

void feedDigitBlocks(GoertzelStream& goertzel,
                     char digit,
                     int blocks,
                     float& phaseOffset,
                     float lowAmplitude = 12000.0f,
                     float highAmplitude = 12000.0f) {
    int row = -1;
    int col = -1;
    TEST_ASSERT_TRUE_MESSAGE(mapDigitToRowCol(digit, row, col), "Digit not mapped to row/col");

    const PhoneConfig& config = getPhoneConfig();
    const int blockSize = config.goertzelBlockSize;
    std::vector<int16_t> toneBlock(blockSize);

    generateDualToneBlockWithGainsForTest(
        toneBlock.data(),
        toneBlock.size(),
        static_cast<float>(AUDIO_SAMPLE_RATE),
        config.rowFreqs[row],
        config.colFreqs[col],
        lowAmplitude,
        highAmplitude,
        phaseOffset
    );

    for (int i = 0; i < blocks; i++) {
        processGoertzelSamplesForTest(goertzel, toneBlock.data(), toneBlock.size());
        runMainLikeSequenceStep();
        phaseOffset += static_cast<float>(blockSize);
    }
}

void emitDigitFromFrequencies(GoertzelStream& goertzel, char digit, float& phaseOffset) {
    const PhoneConfig& config = getPhoneConfig();
    feedDigitBlocks(goertzel, digit, config.requiredConsecutive, phaseOffset);
    feedSilenceBlocks(goertzel, GOERTZEL_RELEASE_BLOCKS_FOR_TEST + 2, phaseOffset);
}

void configureDefaultHookCallbacks() {
    Phone.setHookCallback([](bool isOffHook) {
        if (isOffHook) {
            audioPlayer.playAudioKey("dialtone");
        } else {
            audioPlayer.stop();
            resetDTMFSequence();
        }
    });
}

}  // namespace

void setUp() {
    resetGoertzelState();
    resetDTMFSequence();
    clearRegistryForTest();
    resetAudioPlayerSpyState();
    setPlayPlaylistResultForTest(false);
    setMaxSequenceLength(0);

    Phone.setHookCallback(HookStateCallback());
    Phone.setOffHook(false, true);
}

void tearDown() {}

void test_offhook_callback_plays_dialtone() {
    configureDefaultHookCallbacks();

    Phone.setOffHook(true, true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_EQUAL(1, spy.playAudioKeyCalls);
    TEST_ASSERT_EQUAL_STRING("dialtone", spy.lastAudioKey);
    TEST_ASSERT_TRUE(spy.active);
}

void test_offhook_and_frequency_sequence_triggers_registered_key() {
    registerMockAudioKeyForTest("911");
    configureDefaultHookCallbacks();

    Phone.setOffHook(true, true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_EQUAL_STRING("dialtone", spy.lastAudioKey);

    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);

    float phaseOffset = 0.0f;
    emitDigitFromFrequencies(goertzel, '9', phaseOffset);
    emitDigitFromFrequencies(goertzel, '1', phaseOffset);
    emitDigitFromFrequencies(goertzel, '1', phaseOffset);

    TEST_ASSERT_GREATER_THAN(0, spy.stopCalls);
    TEST_ASSERT_GREATER_THAN(0, spy.playPlaylistCalls);
    TEST_ASSERT_EQUAL_STRING("911", spy.lastAudioKey);
}

void test_direct_digits_with_hash_terminator_trigger_registered_key() {
    registerMockAudioKeyForTest("45");

    addDtmfDigit('4');
    addDtmfDigit('5');
    addDtmfDigit('#');

    bool started = readDTMFSequence(true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_GREATER_THAN(0, spy.playPlaylistCalls);
    TEST_ASSERT_EQUAL_STRING("45", spy.lastAudioKey);
    TEST_ASSERT_FALSE(isReadingSequence());
}

void test_substring_match_9911_triggers_911() {
    registerMockAudioKeyForTest("911");

    addDtmfDigit('9');
    addDtmfDigit('9');
    addDtmfDigit('1');
    addDtmfDigit('1');

    bool started = readDTMFSequence(true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_STRING("911", spy.lastAudioKey);
}

void test_unknown_direct_sequence_plays_wrong_number() {
    addDtmfDigit('1');
    addDtmfDigit('2');
    addDtmfDigit('3');
    addDtmfDigit('*');

    bool started = readDTMFSequence(true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_STRING("wrong_number", spy.lastAudioKey);
}

void test_invalid_digit_is_ignored() {
    addDtmfDigit('A');
    addDtmfDigit('x');

    TEST_ASSERT_EQUAL_STRING("", getSequence());
    TEST_ASSERT_FALSE(isReadingSequence());
    TEST_ASSERT_FALSE(isSequenceReady());
}

void test_goertzel_requires_requiredConsecutive_blocks() {
    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);
    float phaseOffset = 0.0f;
    const PhoneConfig& config = getPhoneConfig();

    for (int i = 0; i < config.requiredConsecutive - 1; i++) {
        feedDigitBlocks(goertzel, '5', 1, phaseOffset);
        TEST_ASSERT_EQUAL(0, getGoertzelKey());
    }

    feedDigitBlocks(goertzel, '5', 1, phaseOffset);
    TEST_ASSERT_EQUAL('5', getGoertzelKey());
}

void test_goertzel_suppresses_repeat_until_release() {
    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);
    float phaseOffset = 0.0f;
    const PhoneConfig& config = getPhoneConfig();

    feedDigitBlocks(goertzel, '8', config.requiredConsecutive, phaseOffset);
    TEST_ASSERT_EQUAL('8', getGoertzelKey());

    feedDigitBlocks(goertzel, '8', 1, phaseOffset);
    TEST_ASSERT_EQUAL(0, getGoertzelKey());

    feedSilenceBlocks(goertzel, GOERTZEL_RELEASE_BLOCKS_FOR_TEST, phaseOffset);
    TEST_ASSERT_EQUAL(0, getGoertzelKey());

    feedDigitBlocks(goertzel, '8', config.requiredConsecutive, phaseOffset);
    TEST_ASSERT_EQUAL('8', getGoertzelKey());
}

void test_goertzel_rejects_single_tone_blocks() {
    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);
    const PhoneConfig& config = getPhoneConfig();
    const int blockSize = config.goertzelBlockSize;
    std::vector<int16_t> singleTone(blockSize);

    float phaseOffset = 0.0f;
    for (int i = 0; i < config.requiredConsecutive + 1; i++) {
        generateSingleToneBlockForTest(
            singleTone.data(),
            singleTone.size(),
            static_cast<float>(AUDIO_SAMPLE_RATE),
            config.rowFreqs[1],
            14000.0f,
            phaseOffset
        );
        processGoertzelSamplesForTest(goertzel, singleTone.data(), singleTone.size());
        phaseOffset += static_cast<float>(blockSize);
        TEST_ASSERT_EQUAL(0, getGoertzelKey());
    }
}

void test_goertzel_rejects_excessive_twist_ratio() {
    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);
    int row = -1;
    int col = -1;
    TEST_ASSERT_TRUE(mapDigitToRowCol('6', row, col));

    const PhoneConfig& config = getPhoneConfig();
    const int blockSize = config.goertzelBlockSize;
    std::vector<int16_t> toneBlock(blockSize);

    float phaseOffset = 0.0f;
    for (int i = 0; i < config.requiredConsecutive + 1; i++) {
        generateDualToneBlockWithGainsForTest(
            toneBlock.data(),
            toneBlock.size(),
            static_cast<float>(AUDIO_SAMPLE_RATE),
            config.rowFreqs[row],
            config.colFreqs[col],
            450.0f,
            12000.0f,
            phaseOffset
        );
        processGoertzelSamplesForTest(goertzel, toneBlock.data(), toneBlock.size());
        phaseOffset += static_cast<float>(blockSize);
        TEST_ASSERT_EQUAL(0, getGoertzelKey());
    }
}

void test_onhook_callback_resets_sequence() {
    configureDefaultHookCallbacks();

    Phone.setOffHook(true, true);
    addDtmfDigit('9');
    addDtmfDigit('1');
    TEST_ASSERT_EQUAL_STRING("91", getSequence());

    Phone.setOffHook(false, true);
    TEST_ASSERT_EQUAL_STRING("", getSequence());
    TEST_ASSERT_FALSE(isReadingSequence());
}

void test_known_sequence_can_use_playlist_success_without_fallback() {
    registerMockAudioKeyForTest("123");
    setPlayPlaylistResultForTest(true);

    addDtmfDigit('1');
    addDtmfDigit('2');
    addDtmfDigit('3');
    bool started = readDTMFSequence(true);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_GREATER_THAN(0, spy.playPlaylistCalls);
    TEST_ASSERT_EQUAL(0, spy.playAudioKeyCalls);
}

void test_max_sequence_length_configuration_roundtrip() {
    setMaxSequenceLength(4);
    TEST_ASSERT_EQUAL(4, getMaxSequenceLength());

    setMaxSequenceLength(0);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LENGTH, getMaxSequenceLength());

    setMaxSequenceLength(MAX_SEQUENCE_LENGTH + 5);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LENGTH, getMaxSequenceLength());
}

void test_unknown_frequency_sequence_plays_wrong_number() {
    configureDefaultHookCallbacks();

    Phone.setOffHook(true, true);

    GoertzelStream goertzel;
    initGoertzelForTest(goertzel);

    float phaseOffset = 0.0f;
    emitDigitFromFrequencies(goertzel, '1', phaseOffset);
    emitDigitFromFrequencies(goertzel, '2', phaseOffset);
    emitDigitFromFrequencies(goertzel, '3', phaseOffset);
    emitDigitFromFrequencies(goertzel, '*', phaseOffset);

    AudioPlayerSpyState& spy = getAudioPlayerSpyState();
    TEST_ASSERT_EQUAL_STRING("wrong_number", spy.lastAudioKey);
}

void setup() {
    delay(1000);
    UNITY_BEGIN();
    RUN_TEST(test_offhook_callback_plays_dialtone);
    RUN_TEST(test_offhook_and_frequency_sequence_triggers_registered_key);
    RUN_TEST(test_direct_digits_with_hash_terminator_trigger_registered_key);
    RUN_TEST(test_substring_match_9911_triggers_911);
    RUN_TEST(test_unknown_direct_sequence_plays_wrong_number);
    RUN_TEST(test_invalid_digit_is_ignored);
    RUN_TEST(test_goertzel_requires_requiredConsecutive_blocks);
    RUN_TEST(test_goertzel_suppresses_repeat_until_release);
    RUN_TEST(test_goertzel_rejects_single_tone_blocks);
    RUN_TEST(test_goertzel_rejects_excessive_twist_ratio);
    RUN_TEST(test_onhook_callback_resets_sequence);
    RUN_TEST(test_known_sequence_can_use_playlist_success_without_fallback);
    RUN_TEST(test_max_sequence_length_configuration_roundtrip);
    RUN_TEST(test_unknown_frequency_sequence_plays_wrong_number);
    UNITY_END();
}

void loop() {}

#else

void setup() {}
void loop() {}

#endif // TEST_MODE

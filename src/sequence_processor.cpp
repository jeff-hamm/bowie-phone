#include "sequence_processor.h"
#include "known_processor.h"

// ============================================================================
// MAIN PROCESSING FUNCTIONS
// ============================================================================

const char* processNumberSequence(const char *sequence)
{
    Serial.printf("=== Processing DTMF Sequence: '%s' (length: %d) ===\n",
                  sequence, strlen(sequence));

    // Check sequence type and route to appropriate handler
    const char* audioPath = nullptr;
    
    if (isSpecialCommand(sequence))
    {
        processSpecialCommand(sequence);
    }
    else if (isKnownSequence(sequence))
    {
        audioPath = processKnownSequence(sequence);
    }
    else
    {
        processUnknownSequence(sequence);
    }

    Serial.printf("=== Sequence Processing Complete ===\n\n");
    return audioPath;
}

void processUnknownSequence(const char *sequence)
{
    Serial.printf("❓ UNKNOWN SEQUENCE: %s\n", sequence);
    Serial.printf("💡 This sequence doesn't match any known patterns\n");

    // Unknown sequence handling:
    // - Log for analysis
    // - Check for patterns
    // - Provide user feedback
    // - Suggest alternatives
}

// ============================================================================
// PHONE NUMBER PROCESSING FUNCTIONS
// ============================================================================
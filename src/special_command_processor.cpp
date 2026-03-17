// All implementation has moved to src/commands/*.cpp
// This file is kept as an empty translation unit to avoid breaking
// any build rules that may reference it directly.
//
// Public API:  include/special_command_processor.h  (unchanged)
// Source tree: src/commands/
//   commands_internal.h   - shared includes for commands/ only
//   debug_commands.cpp    - processDebugInput, enterFirmwareUpdateMode, shutdownAudioForOTA,
//                           NVS capture state, processDebugCommand dispatch
//   special_commands.cpp  - initializeSpecialCommands, EEPROM, execute*() handlers,
//                           isSpecialCommand, processSpecialCommand
//   cpu_load_test.cpp     - performGoertzelCPULoadTest
//   audio_capture.cpp     - performAudioCapture
//   audio_output_test.cpp - performAudioOutputTest
//   debug_input.cpp       - performDebugInput + helpers (pumpMainLoop, etc.)
//   sd_card_debug.cpp     - performSDCardDebug

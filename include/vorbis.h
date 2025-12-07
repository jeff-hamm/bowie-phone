/**
 * @file vorbis.h
 * @brief Wrapper header for libvorbis-tremor compatibility
 * 
 * This header provides the vorbis.h interface expected by audio-tools
 * VorbisDecoder using the tremor (integer-only) implementation.
 */
#pragma once

// Include the tremor library headers
#include "tremor/ivorbiscodec.h"
#include "tremor/ivorbisfile.h"

// The VorbisDecoder.h expects these types and functions from vorbis.h
// Tremor uses the same API but with integer-only implementation

// Compatibility typedefs - tremor uses the same names
// vorbis_info, vorbis_comment, vorbis_dsp_state, vorbis_block are defined in ivorbiscodec.h

// Function compatibility - these are the same in tremor
// vorbis_info_init, vorbis_comment_init, vorbis_synthesis_init, etc.
// are all defined in ivorbiscodec.h

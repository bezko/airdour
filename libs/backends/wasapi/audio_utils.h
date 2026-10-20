/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Interleave helpers for the WASAPI shared-mode convert/mix path.
 */

#pragma once

#include <stdint.h>

inline void
wasapi_deinterleave (const float* interleaved_input,
                     float* output,
                     uint32_t sample_count,
                     uint32_t channel,
                     uint32_t channel_count)
{
	const float* ptr = interleaved_input + channel;
	while (sample_count-- > 0) {
		*output++ = *ptr;
		ptr += channel_count;
	}
}

inline void
wasapi_interleave (const float* input,
                   float* interleaved_output,
                   uint32_t sample_count,
                   uint32_t channel,
                   uint32_t channel_count)
{
	float* ptr = interleaved_output + channel;
	while (sample_count-- > 0) {
		*ptr = *input++;
		ptr += channel_count;
	}
}

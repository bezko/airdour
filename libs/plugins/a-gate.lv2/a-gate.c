/*
 * Copyright (C) 2026 Airdour contributors
 *
 * ACE Gate — DSP-only noise gate (Threshold + Hold). No Cairo / GTK.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/core/lv2.h>
#else
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#endif

#ifdef COMPILER_MSVC
#include <float.h>
#define isfinite_local(val) (bool)_finite((double)val)
#else
#define isfinite_local isfinite
#endif

#define AGATE_URI "urn:ardour:a-gate"

typedef enum {
	AGATE_THRESHOLD = 0,
	AGATE_HOLD,
	AGATE_ENABLE,
	AGATE_INPUT,
	AGATE_OUTPUT
} PortIndex;

typedef struct {
	float* threshold;
	float* hold;
	float* enable;
	float* input;
	float* output;
	float  srate;
	float  env;
	float  gain;
	float  hold_left; /* samples remaining while held open */
} AGate;

static LV2_Handle
instantiate (const LV2_Descriptor* descriptor,
             double rate,
             const char* bundle_path,
             const LV2_Feature* const* features)
{
	(void)descriptor;
	(void)bundle_path;
	(void)features;
	AGate* self = (AGate*)calloc (1, sizeof (AGate));
	if (!self) {
		return NULL;
	}
	self->srate = (float)rate;
	self->env = 0.f;
	self->gain = 1.f;
	self->hold_left = 0.f;
	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance, uint32_t port, void* data)
{
	AGate* self = (AGate*)instance;
	switch ((PortIndex)port) {
	case AGATE_THRESHOLD:
		self->threshold = (float*)data;
		break;
	case AGATE_HOLD:
		self->hold = (float*)data;
		break;
	case AGATE_ENABLE:
		self->enable = (float*)data;
		break;
	case AGATE_INPUT:
		self->input = (float*)data;
		break;
	case AGATE_OUTPUT:
		self->output = (float*)data;
		break;
	}
}

static void
activate (LV2_Handle instance)
{
	AGate* self = (AGate*)instance;
	self->env = 0.f;
	self->gain = 1.f;
	self->hold_left = 0.f;
}

static inline float
from_db (float db)
{
	return powf (10.f, 0.05f * db);
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	AGate* self = (AGate*)instance;
	const float* in = self->input;
	float* out = self->output;
	if (!in || !out) {
		return;
	}

	const int enabled = (!self->enable || *self->enable > 0.5f) ? 1 : 0;
	float thr_db = self->threshold ? *self->threshold : -40.f;
	if (thr_db < -60.f) {
		thr_db = -60.f;
	}
	if (thr_db > 0.f) {
		thr_db = 0.f;
	}
	const float thr = from_db (thr_db);

	float hold_ms = self->hold ? *self->hold : 50.f;
	if (hold_ms < 1.f) {
		hold_ms = 1.f;
	}
	if (hold_ms > 500.f) {
		hold_ms = 500.f;
	}
	const float hold_samples = hold_ms * 0.001f * self->srate;

	/* Fast attack / medium release for envelope + gate gain. */
	const float atk_coeff = expf (-1000.f / (1.f * self->srate));
	const float rel_coeff = expf (-1000.f / (50.f * self->srate));
	const float open_coeff = expf (-1000.f / (1.f * self->srate));
	const float close_coeff = expf (-1000.f / (10.f * self->srate));

	float env = self->env;
	float gain = self->gain;
	float hold_left = self->hold_left;

	for (uint32_t i = 0; i < n_samples; ++i) {
		float x = in[i];
		if (!isfinite_local (x)) {
			x = 0.f;
		}
		if (!enabled) {
			out[i] = x;
			continue;
		}

		float absx = fabsf (x);
		if (absx > env) {
			env = atk_coeff * env + (1.f - atk_coeff) * absx;
		} else {
			env = rel_coeff * env + (1.f - rel_coeff) * absx;
		}

		float target = 0.f;
		if (env >= thr) {
			target = 1.f;
			hold_left = hold_samples;
		} else if (hold_left > 0.f) {
			target = 1.f;
			hold_left -= 1.f;
		} else {
			target = 0.f;
		}

		if (target > gain) {
			gain = open_coeff * gain + (1.f - open_coeff) * target;
		} else {
			gain = close_coeff * gain + (1.f - close_coeff) * target;
		}
		out[i] = x * gain;
	}

	self->env = env;
	self->gain = gain;
	self->hold_left = hold_left < 0.f ? 0.f : hold_left;
}

static void
cleanup (LV2_Handle instance)
{
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	(void)uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	AGATE_URI,
	instantiate,
	connect_port,
	activate,
	run,
	NULL,
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
	case 0:
		return &descriptor;
	default:
		return NULL;
	}
}

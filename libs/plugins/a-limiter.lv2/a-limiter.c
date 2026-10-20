/*
 * Copyright (C) 2026 Airdour contributors
 *
 * ACE Limiter — DSP-only peak limiter (Ceiling + Release). No Cairo / GTK.
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

#define ALIMITER_URI "urn:ardour:a-limiter"

typedef enum {
	ALIMITER_CEILING = 0,
	ALIMITER_RELEASE,
	ALIMITER_ENABLE,
	ALIMITER_INPUT,
	ALIMITER_OUTPUT
} PortIndex;

typedef struct {
	float* ceiling;
	float* release;
	float* enable;
	float* input;
	float* output;
	float  srate;
	float  env;
	float  gain;
} ALimiter;

static LV2_Handle
instantiate (const LV2_Descriptor* descriptor,
             double rate,
             const char* bundle_path,
             const LV2_Feature* const* features)
{
	(void)descriptor;
	(void)bundle_path;
	(void)features;
	ALimiter* self = (ALimiter*)calloc (1, sizeof (ALimiter));
	if (!self) {
		return NULL;
	}
	self->srate = (float)rate;
	self->env = 0.f;
	self->gain = 1.f;
	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance, uint32_t port, void* data)
{
	ALimiter* self = (ALimiter*)instance;
	switch ((PortIndex)port) {
	case ALIMITER_CEILING:
		self->ceiling = (float*)data;
		break;
	case ALIMITER_RELEASE:
		self->release = (float*)data;
		break;
	case ALIMITER_ENABLE:
		self->enable = (float*)data;
		break;
	case ALIMITER_INPUT:
		self->input = (float*)data;
		break;
	case ALIMITER_OUTPUT:
		self->output = (float*)data;
		break;
	}
}

static void
activate (LV2_Handle instance)
{
	ALimiter* self = (ALimiter*)instance;
	self->env = 0.f;
	self->gain = 1.f;
}

static inline float
from_db (float db)
{
	return powf (10.f, 0.05f * db);
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	ALimiter* self = (ALimiter*)instance;
	const float* in = self->input;
	float* out = self->output;
	if (!in || !out) {
		return;
	}

	const int enabled = (!self->enable || *self->enable > 0.5f) ? 1 : 0;
	float ceiling_db = self->ceiling ? *self->ceiling : -0.3f;
	if (ceiling_db < -20.f) {
		ceiling_db = -20.f;
	}
	if (ceiling_db > 0.f) {
		ceiling_db = 0.f;
	}
	const float ceiling = from_db (ceiling_db);

	float release_ms = self->release ? *self->release : 100.f;
	if (release_ms < 1.f) {
		release_ms = 1.f;
	}
	if (release_ms > 2000.f) {
		release_ms = 2000.f;
	}
	const float rel_coeff = expf (-1000.f / (release_ms * self->srate));
	const float atk_coeff = expf (-1000.f / (0.5f * self->srate)); /* ~0.5 ms attack */

	float env = self->env;
	float gain = self->gain;

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

		float target = 1.f;
		if (env > ceiling && env > 1.0e-12f) {
			target = ceiling / env;
		}
		/* Smooth gain changes with release for drops, attack for rises. */
		if (target < gain) {
			gain = atk_coeff * gain + (1.f - atk_coeff) * target;
		} else {
			gain = rel_coeff * gain + (1.f - rel_coeff) * target;
		}
		out[i] = x * gain;
	}

	self->env = env;
	self->gain = gain;
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
	ALIMITER_URI,
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

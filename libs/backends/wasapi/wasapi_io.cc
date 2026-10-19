/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "wasapi_io.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

struct WasapiIO::ComState {
	IMMDeviceEnumerator* enumerator;
	IAudioClient*        render_client;
	IAudioClient*        capture_client;
	IAudioRenderClient*  render;
	IAudioCaptureClient* capture;
	HANDLE               render_event;
	WAVEFORMATEX*        render_mix;
	WAVEFORMATEX*        capture_mix;
	UINT32               render_buffer_frames;
	bool                 com_inited_here;
	bool                 capture_enabled;
	std::vector<float>   capture_scratch; /* pre-sized; no RT alloc in transfer */

	ComState ()
		: enumerator (0)
		, render_client (0)
		, capture_client (0)
		, render (0)
		, capture (0)
		, render_event (NULL)
		, render_mix (0)
		, capture_mix (0)
		, render_buffer_frames (0)
		, com_inited_here (false)
		, capture_enabled (false)
	{}
};

static std::string
wide_to_utf8 (LPCWSTR w)
{
	if (!w) {
		return std::string ();
	}
	int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 1) {
		return std::string ();
	}
	std::string out (n - 1, '\0');
	WideCharToMultiByte (CP_UTF8, 0, w, -1, &out[0], n, NULL, NULL);
	return out;
}

static uint32_t
hns_to_frames (REFERENCE_TIME hns, float sr)
{
	if (hns <= 0 || sr <= 0.f) {
		return 0;
	}
	/* REFERENCE_TIME is 100ns units */
	double frames = (double)hns * (double)sr / 10000000.0;
	return (uint32_t)std::max (1.0, std::floor (frames + 0.5));
}

static void
release_format (WAVEFORMATEX*& fmt)
{
	if (fmt) {
		CoTaskMemFree (fmt);
		fmt = 0;
	}
}

static bool
format_is_float (const WAVEFORMATEX* fmt)
{
	if (!fmt) {
		return false;
	}
	if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
		return true;
	}
	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
		return IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? true : false;
	}
	return false;
}

static uint32_t
format_bits (const WAVEFORMATEX* fmt)
{
	if (!fmt) {
		return 0;
	}
	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
		if (ext->Samples.wValidBitsPerSample) {
			return ext->Samples.wValidBitsPerSample;
		}
	}
	return fmt->wBitsPerSample;
}

static void
convert_from_device (const WAVEFORMATEX* fmt, const BYTE* src, float* dst, UINT32 frames, UINT32 chans)
{
	if (!src || !dst || !frames || !chans) {
		return;
	}
	if (format_is_float (fmt) && format_bits (fmt) == 32) {
		memcpy (dst, src, frames * chans * sizeof (float));
		return;
	}
	const uint32_t bits = format_bits (fmt);
	if (bits == 16) {
		const int16_t* s = (const int16_t*)src;
		const float scale = 1.f / 32768.f;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			dst[i] = (float)s[i] * scale;
		}
		return;
	}
	if (bits == 24) {
		/* packed 24-bit little-endian */
		const BYTE* p = src;
		const float scale = 1.f / 8388608.f;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			int32_t v = (int32_t)((p[0]) | (p[1] << 8) | (p[2] << 16));
			if (v & 0x800000) {
				v |= ~0xffffff;
			}
			dst[i] = (float)v * scale;
			p += 3;
		}
		return;
	}
	if (bits == 32 && !format_is_float (fmt)) {
		const int32_t* s = (const int32_t*)src;
		const float scale = 1.f / 2147483648.f;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			dst[i] = (float)s[i] * scale;
		}
		return;
	}
	memset (dst, 0, frames * chans * sizeof (float));
}

static void
convert_to_device (const WAVEFORMATEX* fmt, const float* src, BYTE* dst, UINT32 frames, UINT32 chans)
{
	if (!dst || !frames || !chans) {
		return;
	}
	if (!src) {
		UINT32 bytes = frames * chans * sizeof (float);
		if (fmt && fmt->nChannels) {
			bytes = frames * fmt->nBlockAlign;
		}
		memset (dst, 0, bytes);
		return;
	}
	if (format_is_float (fmt) && format_bits (fmt) == 32) {
		memcpy (dst, src, frames * chans * sizeof (float));
		return;
	}
	const uint32_t bits = format_bits (fmt);
	if (bits == 16) {
		int16_t* d = (int16_t*)dst;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			float x = src[i];
			if (x > 1.f) x = 1.f;
			if (x < -1.f) x = -1.f;
			d[i] = (int16_t)lrintf (x * 32767.f);
		}
		return;
	}
	if (bits == 24) {
		BYTE* p = dst;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			float x = src[i];
			if (x > 1.f) x = 1.f;
			if (x < -1.f) x = -1.f;
			int32_t v = (int32_t)lrintf (x * 8388607.f);
			p[0] = (BYTE)(v & 0xff);
			p[1] = (BYTE)((v >> 8) & 0xff);
			p[2] = (BYTE)((v >> 16) & 0xff);
			p += 3;
		}
		return;
	}
	if (bits == 32 && !format_is_float (fmt)) {
		int32_t* d = (int32_t*)dst;
		const UINT32 n = frames * chans;
		for (UINT32 i = 0; i < n; ++i) {
			float x = src[i];
			if (x > 1.f) x = 1.f;
			if (x < -1.f) x = -1.f;
			d[i] = (int32_t)lrintf (x * 2147483647.f);
		}
		return;
	}
	memset (dst, 0, frames * chans * sizeof (float));
}

static HRESULT
activate_client (IMMDeviceEnumerator* enumerator,
                 const std::string& device_id,
                 EDataFlow flow,
                 IAudioClient** out_client,
                 WAVEFORMATEX** out_mix,
                 IMMDevice** out_dev_optional)
{
	*out_client = 0;
	*out_mix = 0;
	IMMDevice* dev = 0;
	HRESULT hr;
	if (device_id.empty ()) {
		hr = enumerator->GetDefaultAudioEndpoint (flow, eConsole, &dev);
	} else {
		int wlen = MultiByteToWideChar (CP_UTF8, 0, device_id.c_str (), -1, NULL, 0);
		std::wstring wid (wlen, L'\0');
		MultiByteToWideChar (CP_UTF8, 0, device_id.c_str (), -1, &wid[0], wlen);
		hr = enumerator->GetDevice (wid.c_str (), &dev);
	}
	if (FAILED (hr) || !dev) {
		return hr;
	}
	hr = dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, NULL, (void**)out_client);
	if (FAILED (hr)) {
		dev->Release ();
		return hr;
	}
	hr = (*out_client)->GetMixFormat (out_mix);
	if (FAILED (hr)) {
		(*out_client)->Release ();
		*out_client = 0;
		dev->Release ();
		return hr;
	}
	if (out_dev_optional) {
		*out_dev_optional = dev;
	} else {
		dev->Release ();
	}
	return S_OK;
}

#endif /* _WIN32 */

WasapiIO::WasapiIO ()
	: _open (false)
	, _sample_rate (48000.f)
	, _period_frames (512)
	, _default_period_frames (480)
	, _min_period_frames (128)
	, _capture_channels (0)
	, _playback_channels (0)
	, _mix_bits (32)
	, _mix_is_float (true)
#ifdef _WIN32
	, _com (0)
#endif
{
}

WasapiIO::~WasapiIO ()
{
	close ();
}

void
WasapiIO::recommended_buffer_sizes (std::vector<uint32_t>& sizes) const
{
	sizes.clear ();
	/* Prefer multiples of the shared quantum; keep laptop-friendly range. */
	const uint32_t q = std::max (64u, _min_period_frames ? _min_period_frames : 64u);
	const uint32_t candidates[] = {
		q,
		q * 2,
		q * 3,
		q * 4,
		q * 6,
		q * 8,
		256u, 512u, 1024u, 2048u
	};
	for (unsigned i = 0; i < sizeof (candidates) / sizeof (candidates[0]); ++i) {
		uint32_t v = candidates[i];
		if (v < 64 || v > 4096) {
			continue;
		}
		if (std::find (sizes.begin (), sizes.end (), v) == sizes.end ()) {
			sizes.push_back (v);
		}
	}
	std::sort (sizes.begin (), sizes.end ());
}

std::string
WasapiIO::resolve_device_id (const std::string& name_or_id, bool want_input) const
{
	if (name_or_id.empty () || name_or_id == "Default" || name_or_id == "default") {
		return std::string ();
	}
	for (std::vector<DeviceInfo>::const_iterator i = _cached_devices.begin ();
	     i != _cached_devices.end (); ++i) {
		if (i->id == name_or_id) {
			return i->id;
		}
	}
	for (std::vector<DeviceInfo>::const_iterator i = _cached_devices.begin ();
	     i != _cached_devices.end (); ++i) {
		if (i->name == name_or_id) {
			if ((want_input && i->is_input) || (!want_input && i->is_output)) {
				return i->id;
			}
		}
	}
	/* Fall back: treat the string as a raw device id. */
	return name_or_id;
}

int
WasapiIO::enumerate_devices (std::vector<DeviceInfo>& devices)
{
	devices.clear ();
	_cached_devices.clear ();

#ifndef _WIN32
	return -1;
#else
	HRESULT hr;
	bool release_enumerator = false;
	IMMDeviceEnumerator* enumerator = 0;

	hr = CoInitializeEx (NULL, COINIT_MULTITHREADED);
	const bool com_here = (hr == S_OK || hr == S_FALSE);
	if (FAILED (hr) && hr != RPC_E_CHANGED_MODE) {
		return -1;
	}

	hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), NULL, CLSCTX_ALL,
	                       __uuidof (IMMDeviceEnumerator), (void**)&enumerator);
	if (FAILED (hr) || !enumerator) {
		if (com_here) {
			CoUninitialize ();
		}
		return -1;
	}
	release_enumerator = true;

	IMMDevice* def_render = 0;
	IMMDevice* def_capture = 0;
	LPWSTR def_render_id = 0;
	LPWSTR def_capture_id = 0;
	enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &def_render);
	enumerator->GetDefaultAudioEndpoint (eCapture, eConsole, &def_capture);
	if (def_render) {
		def_render->GetId (&def_render_id);
	}
	if (def_capture) {
		def_capture->GetId (&def_capture_id);
	}

	EDataFlow flows[2] = { eRender, eCapture };
	for (int f = 0; f < 2; ++f) {
		IMMDeviceCollection* coll = 0;
		hr = enumerator->EnumAudioEndpoints (flows[f], DEVICE_STATE_ACTIVE, &coll);
		if (FAILED (hr) || !coll) {
			continue;
		}
		UINT count = 0;
		coll->GetCount (&count);
		for (UINT i = 0; i < count; ++i) {
			IMMDevice* dev = 0;
			if (FAILED (coll->Item (i, &dev)) || !dev) {
				continue;
			}
			LPWSTR idw = 0;
			dev->GetId (&idw);
			IPropertyStore* props = 0;
			std::string name = "Audio Device";
			if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &props)) && props) {
				PROPVARIANT var;
				PropVariantInit (&var);
				if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
					name = wide_to_utf8 (var.pwszVal);
				}
				PropVariantClear (&var);
				props->Release ();
			}
			DeviceInfo info;
			info.id = wide_to_utf8 (idw);
			info.name = name;
			info.is_input = (flows[f] == eCapture);
			info.is_output = (flows[f] == eRender);
			info.is_default_input = (def_capture_id && idw && wcscmp (idw, def_capture_id) == 0);
			info.is_default_output = (def_render_id && idw && wcscmp (idw, def_render_id) == 0);
			info.available = true;
			devices.push_back (info);
			if (idw) {
				CoTaskMemFree (idw);
			}
			dev->Release ();
		}
		coll->Release ();
	}

	if (def_render_id) {
		CoTaskMemFree (def_render_id);
	}
	if (def_capture_id) {
		CoTaskMemFree (def_capture_id);
	}
	if (def_render) {
		def_render->Release ();
	}
	if (def_capture) {
		def_capture->Release ();
	}
	if (release_enumerator) {
		enumerator->Release ();
	}
	if (com_here) {
		CoUninitialize ();
	}

	_cached_devices = devices;
	return 0;
#endif
}

int
WasapiIO::open (const std::string& output_name_or_id,
                const std::string& input_name_or_id,
                float             /*sample_rate_hint*/,
                uint32_t          frames_per_period)
{
	close ();

#ifndef _WIN32
	(void)output_name_or_id;
	(void)input_name_or_id;
	(void)frames_per_period;
	return -1;
#else
	if (_cached_devices.empty ()) {
		std::vector<DeviceInfo> tmp;
		enumerate_devices (tmp);
	}

	_com = new ComState ();

	HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED);
	if (hr == S_OK || hr == S_FALSE) {
		_com->com_inited_here = true;
	} else if (hr != RPC_E_CHANGED_MODE) {
		delete _com;
		_com = 0;
		return -1;
	}

	hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), NULL, CLSCTX_ALL,
	                       __uuidof (IMMDeviceEnumerator), (void**)&_com->enumerator);
	if (FAILED (hr) || !_com->enumerator) {
		close ();
		return -1;
	}

	const std::string out_id = resolve_device_id (output_name_or_id, false);
	const std::string in_id = resolve_device_id (input_name_or_id, true);

	hr = activate_client (_com->enumerator, out_id, eRender,
	                      &_com->render_client, &_com->render_mix, NULL);
	if (FAILED (hr) || !_com->render_client || !_com->render_mix) {
		close ();
		return -1;
	}

	_sample_rate = (float)_com->render_mix->nSamplesPerSec;
	_playback_channels = _com->render_mix->nChannels;
	_mix_is_float = format_is_float (_com->render_mix);
	_mix_bits = format_bits (_com->render_mix);

	REFERENCE_TIME def_period = 0, min_period = 0;
	_com->render_client->GetDevicePeriod (&def_period, &min_period);
	_default_period_frames = hns_to_frames (def_period, _sample_rate);
	_min_period_frames = hns_to_frames (min_period, _sample_rate);
	if (_min_period_frames == 0) {
		_min_period_frames = 128;
	}
	if (_default_period_frames == 0) {
		_default_period_frames = std::max (_min_period_frames * 2, 480u);
	}

	/* Prefer Win10 GetSharedModeEnginePeriod when present. */
	UINT32 default_period = 0, fundamental = 0, min_period_f = 0, max_period_f = 0;
	IAudioClient3* ac3 = 0;
	if (SUCCEEDED (_com->render_client->QueryInterface (__uuidof (IAudioClient3), (void**)&ac3)) && ac3) {
		if (SUCCEEDED (ac3->GetSharedModeEnginePeriod (_com->render_mix,
		                                               &default_period, &fundamental,
		                                               &min_period_f, &max_period_f))) {
			if (fundamental) {
				_min_period_frames = fundamental;
			} else if (min_period_f) {
				_min_period_frames = min_period_f;
			}
			if (default_period) {
				_default_period_frames = default_period;
			}
		}
		ac3->Release ();
	}

	/* Prefer ~10 ms shared quantum (480 @ 48 kHz) for laptop stability. */
	const uint32_t ten_ms = (uint32_t)std::lround ((double)_sample_rate * 0.010);
	uint32_t period = frames_per_period ? frames_per_period : _default_period_frames;
	if (frames_per_period == 0 || frames_per_period == 512) {
		/* Migrate historical 512 default → device default or 10 ms. */
		period = _default_period_frames ? _default_period_frames
		                                : (ten_ms ? ten_ms : 480u);
	}
	/* Snap to quantum for stable shared-mode scheduling on cheap laptops. */
	const uint32_t q = std::max (1u, _min_period_frames);
	period = ((period + q / 2) / q) * q;
	if (period < q) {
		period = q;
	}
	/* Soft clamp: very small periods glitch on stock laptop mixers. */
	if (period < 128) {
		period = ((128 + q - 1) / q) * q;
	}
	if (period > 4096) {
		period = (4096 / q) * q;
	}
	_period_frames = period;

	_com->render_event = CreateEvent (NULL, FALSE, FALSE, NULL);
	if (!_com->render_event) {
		close ();
		return -1;
	}

	/*
	 * Shared mode + event callback. Buffer duration 0 → engine default.
	 * AUTOCONVERTPCM helps when requesting rates near mix format on some stacks;
	 * we still adopt mix sample rate above so conversion is usually a no-op.
	 */
	DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	hr = _com->render_client->Initialize (
		AUDCLNT_SHAREMODE_SHARED,
		stream_flags,
		0, /* hnsBufferDuration: shared default */
		0, /* hnsPeriodicity must be 0 in shared */
		_com->render_mix,
		NULL);
	if (FAILED (hr)) {
		close ();
		return -1;
	}

	hr = _com->render_client->SetEventHandle (_com->render_event);
	if (FAILED (hr)) {
		close ();
		return -1;
	}

	hr = _com->render_client->GetBufferSize (&_com->render_buffer_frames);
	if (FAILED (hr)) {
		close ();
		return -1;
	}

	hr = _com->render_client->GetService (__uuidof (IAudioRenderClient), (void**)&_com->render);
	if (FAILED (hr) || !_com->render) {
		close ();
		return -1;
	}

	/* Optional capture — same shared-mode rules; clocked by render event. */
	_com->capture_enabled = false;
	_capture_channels = 0;
	/* Try default capture unless input explicitly disabled with "None". */
	if (input_name_or_id != "None" && input_name_or_id != "none") {
		hr = activate_client (_com->enumerator, in_id, eCapture,
		                      &_com->capture_client, &_com->capture_mix, NULL);
		if (SUCCEEDED (hr) && _com->capture_client && _com->capture_mix) {
			/*
			 * Capture must match render rate for a single-period engine loop.
			 * If mix rates differ, skip capture for now (honest limitation).
			 */
			if (_com->capture_mix->nSamplesPerSec == _com->render_mix->nSamplesPerSec) {
				hr = _com->capture_client->Initialize (
					AUDCLNT_SHAREMODE_SHARED,
					0, /* no separate event; pull on render tick */
					0, 0,
					_com->capture_mix,
					NULL);
				if (SUCCEEDED (hr)) {
					hr = _com->capture_client->GetService (
						__uuidof (IAudioCaptureClient), (void**)&_com->capture);
					if (SUCCEEDED (hr) && _com->capture) {
						_capture_channels = _com->capture_mix->nChannels;
						_com->capture_enabled = true;
					}
				}
			}
			if (!_com->capture_enabled) {
				if (_com->capture) {
					_com->capture->Release ();
					_com->capture = 0;
				}
				if (_com->capture_client) {
					_com->capture_client->Release ();
					_com->capture_client = 0;
				}
				release_format (_com->capture_mix);
			}
		}
	}

	hr = _com->render_client->Start ();
	if (FAILED (hr)) {
		close ();
		return -1;
	}
	if (_com->capture_enabled && _com->capture_client) {
		_com->capture_client->Start ();
	}

	if (_com->capture_enabled) {
		_com->capture_scratch.resize (std::max (_period_frames, _com->render_buffer_frames) * std::max (1u, _capture_channels));
	}

	_open = true;
	return 0;
#endif
}

void
WasapiIO::close ()
{
#ifdef _WIN32
	if (_com) {
		if (_com->capture_client) {
			_com->capture_client->Stop ();
		}
		if (_com->render_client) {
			_com->render_client->Stop ();
		}
		if (_com->render) {
			_com->render->Release ();
			_com->render = 0;
		}
		if (_com->capture) {
			_com->capture->Release ();
			_com->capture = 0;
		}
		if (_com->render_client) {
			_com->render_client->Release ();
			_com->render_client = 0;
		}
		if (_com->capture_client) {
			_com->capture_client->Release ();
			_com->capture_client = 0;
		}
		release_format (_com->render_mix);
		release_format (_com->capture_mix);
		if (_com->render_event) {
			CloseHandle (_com->render_event);
			_com->render_event = NULL;
		}
		if (_com->enumerator) {
			_com->enumerator->Release ();
			_com->enumerator = 0;
		}
		if (_com->com_inited_here) {
			CoUninitialize ();
		}
		delete _com;
		_com = 0;
	}
#endif
	_open = false;
	_capture_channels = 0;
	_playback_channels = 0;
}

int
WasapiIO::wait_for_period ()
{
	if (!_open) {
		return -1;
	}
#ifndef _WIN32
	return -1;
#else
	if (!_com || !_com->render_event) {
		return -1;
	}
	DWORD wait = WaitForSingleObject (_com->render_event, 2000);
	if (wait == WAIT_TIMEOUT) {
		return 0;
	}
	if (wait != WAIT_OBJECT_0) {
		return -1;
	}
	return 1;
#endif
}

int
WasapiIO::read_capture (float* interleaved_capture, uint32_t max_frames)
{
	if (!_open) {
		return -1;
	}
#ifndef _WIN32
	(void)interleaved_capture;
	(void)max_frames;
	return -1;
#else
	const uint32_t period = std::min (_period_frames, max_frames ? max_frames : _period_frames);
	if (!interleaved_capture || _capture_channels == 0) {
		return (int)period;
	}
	memset (interleaved_capture, 0, period * _capture_channels * sizeof (float));
	if (!_com || !_com->capture_enabled || !_com->capture) {
		return (int)period;
	}

	UINT32 packet = 0;
	HRESULT hr = _com->capture->GetNextPacketSize (&packet);
	UINT32 written = 0;
	while (SUCCEEDED (hr) && packet > 0 && written < period) {
		BYTE* data = 0;
		UINT32 frames = 0;
		DWORD flags = 0;
		hr = _com->capture->GetBuffer (&data, &frames, &flags, NULL, NULL);
		if (FAILED (hr)) {
			break;
		}
		UINT32 use = frames;
		if (written + use > period) {
			use = period - written;
		}
		if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && use &&
		    _com->capture_scratch.size () >= use * _capture_channels) {
			convert_from_device (_com->capture_mix, data, &_com->capture_scratch[0], use, _capture_channels);
			memcpy (interleaved_capture + written * _capture_channels,
			        &_com->capture_scratch[0],
			        use * _capture_channels * sizeof (float));
		}
		written += use;
		_com->capture->ReleaseBuffer (frames);
		hr = _com->capture->GetNextPacketSize (&packet);
	}
	return (int)period;
#endif
}

int
WasapiIO::write_playback (const float* interleaved_playback, uint32_t max_frames)
{
	if (!_open) {
		return -1;
	}
#ifndef _WIN32
	(void)interleaved_playback;
	(void)max_frames;
	return -1;
#else
	if (!_com || !_com->render_client || !_com->render) {
		return -1;
	}

	const uint32_t period = std::min (_period_frames, max_frames ? max_frames : _period_frames);
	UINT32 padding = 0;
	HRESULT hr = _com->render_client->GetCurrentPadding (&padding);
	if (FAILED (hr)) {
		return -1;
	}
	UINT32 available = (_com->render_buffer_frames > padding)
		? (_com->render_buffer_frames - padding)
		: 0;
	if (available == 0) {
		return -2;
	}

	UINT32 frames = std::min (period, available);
	BYTE* data = 0;
	hr = _com->render->GetBuffer (frames, &data);
	if (FAILED (hr) || !data) {
		return -1;
	}

	convert_to_device (_com->render_mix, interleaved_playback, data, frames, _playback_channels);

	hr = _com->render->ReleaseBuffer (frames, 0);
	if (FAILED (hr)) {
		return -1;
	}
	if (frames < period) {
		return -2;
	}
	return (int)frames;
#endif
}

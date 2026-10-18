/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cstring>

#include "pbd/compose.h"
#include "pbd/error.h"

#include "ardour/audioengine.h"
#include "ardour/debug.h"

#include "wasapi_backend.h"

using namespace ARDOUR;
using namespace PBD;

#ifndef BACKEND_NAME
#define BACKEND_NAME "WASAPI"
#endif

static std::string s_instance_name;

WasapiAudioPort::WasapiAudioPort (WasapiBackend& b, const std::string& name, PortFlags flags)
	: BackendPort (b, name, flags)
{
	memset (_buffer, 0, sizeof (_buffer));
}

WasapiAudioPort::~WasapiAudioPort () {}

void*
WasapiAudioPort::get_buffer (pframes_t /*nframes*/)
{
	return _buffer;
}

WasapiMidiPort::WasapiMidiPort (WasapiBackend& b, const std::string& name, PortFlags flags)
	: BackendPort (b, name, flags)
{
}

WasapiMidiPort::~WasapiMidiPort () {}

void*
WasapiMidiPort::get_buffer (pframes_t /*nframes*/)
{
	return _placeholder;
}

WasapiBackend::WasapiBackend (AudioEngine& e, AudioBackendInfo& info)
	: AudioBackend (e, info)
	, PortEngineSharedImpl (e, s_instance_name.empty () ? "ardour" : s_instance_name)
	, _instance_name (s_instance_name)
	, _samplerate (48000.f)
	, _buffer_size (512)
	, _systemic_input_latency (0)
	, _systemic_output_latency (0)
	, _run (false)
	, _active (false)
	, _freewheeling (false)
	, _dsp_load (0.f)
	, _process_thread (0)
{
}

WasapiBackend::~WasapiBackend ()
{
	stop ();
}

std::string
WasapiBackend::name () const
{
	return BACKEND_NAME;
}

bool
WasapiBackend::is_realtime () const
{
	return true;
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_devices () const
{
	return enumerate_output_devices ();
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_input_devices () const
{
	std::vector<DeviceStatus> r;
	std::vector<WasapiIO::DeviceInfo> devices;
	const_cast<WasapiIO&> (_io).enumerate_devices (devices);
	for (std::vector<WasapiIO::DeviceInfo>::const_iterator i = devices.begin (); i != devices.end (); ++i) {
		/* capture-capable endpoints will be filtered once COM enum lands */
		r.push_back (DeviceStatus (i->name, i->available));
	}
	return r;
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_output_devices () const
{
	std::vector<DeviceStatus> r;
	std::vector<WasapiIO::DeviceInfo> devices;
	const_cast<WasapiIO&> (_io).enumerate_devices (devices);
	for (std::vector<WasapiIO::DeviceInfo>::const_iterator i = devices.begin (); i != devices.end (); ++i) {
		r.push_back (DeviceStatus (i->name, i->available));
	}
	return r;
}

std::vector<float>
WasapiBackend::available_sample_rates (const std::string& /*device*/) const
{
	/* Shared mode follows the device mix format; expose common rates for UI. */
	std::vector<float> rates;
	rates.push_back (44100.f);
	rates.push_back (48000.f);
	rates.push_back (88200.f);
	rates.push_back (96000.f);
	return rates;
}

std::vector<uint32_t>
WasapiBackend::available_buffer_sizes (const std::string& /*device*/) const
{
	/*
	 * Shared-mode periods should align with GetDevicePeriod /
	 * GetSharedModeEnginePeriod. Until COM is wired, expose a laptop-friendly set.
	 */
	std::vector<uint32_t> sizes;
	sizes.push_back (128);
	sizes.push_back (256);
	sizes.push_back (512);
	sizes.push_back (1024);
	sizes.push_back (2048);
	return sizes;
}

int
WasapiBackend::set_device_name (const std::string& name)
{
	_output_device = name;
	_input_device = name;
	return 0;
}

int
WasapiBackend::set_input_device_name (const std::string& name)
{
	_input_device = name;
	return 0;
}

int
WasapiBackend::set_output_device_name (const std::string& name)
{
	_output_device = name;
	return 0;
}

int
WasapiBackend::set_sample_rate (float sr)
{
	if (sr <= 0.f) {
		return -1;
	}
	_samplerate = sr;
	return 0;
}

int
WasapiBackend::set_buffer_size (uint32_t frames)
{
	if (frames < 32) {
		return -1;
	}
	_buffer_size = frames;
	return 0;
}

int
WasapiBackend::set_interleaved (bool /*yn*/)
{
	/* Engine sees non-interleaved ports; device I/O is interleaved internally. */
	return 0;
}

int
WasapiBackend::set_systemic_input_latency (uint32_t l)
{
	_systemic_input_latency = l;
	return 0;
}

int
WasapiBackend::set_systemic_output_latency (uint32_t l)
{
	_systemic_output_latency = l;
	return 0;
}

int
WasapiBackend::set_systemic_midi_input_latency (std::string const, uint32_t)
{
	return 0;
}

int
WasapiBackend::set_systemic_midi_output_latency (std::string const, uint32_t)
{
	return 0;
}

std::string
WasapiBackend::device_name () const
{
	return _output_device;
}

std::string
WasapiBackend::input_device_name () const
{
	return _input_device;
}

std::string
WasapiBackend::output_device_name () const
{
	return _output_device;
}

float
WasapiBackend::sample_rate () const
{
	return _samplerate;
}

uint32_t
WasapiBackend::buffer_size () const
{
	return _buffer_size;
}

bool
WasapiBackend::interleaved () const
{
	return false;
}

uint32_t
WasapiBackend::systemic_input_latency () const
{
	return _systemic_input_latency;
}

uint32_t
WasapiBackend::systemic_output_latency () const
{
	return _systemic_output_latency;
}

uint32_t
WasapiBackend::systemic_midi_input_latency (std::string const) const
{
	return 0;
}

uint32_t
WasapiBackend::systemic_midi_output_latency (std::string const) const
{
	return 0;
}

std::string
WasapiBackend::control_app_name () const
{
	return std::string ();
}

void
WasapiBackend::launch_control_app ()
{
}

std::vector<std::string>
WasapiBackend::enumerate_midi_options () const
{
	std::vector<std::string> opts;
	opts.push_back ("WinMME");
	return opts;
}

int
WasapiBackend::set_midi_option (const std::string&)
{
	return 0;
}

std::string
WasapiBackend::midi_option () const
{
	return "WinMME";
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_midi_devices () const
{
	/* WinMME enumeration lands when MIDI is ported from portaudio backend. */
	return std::vector<DeviceStatus> ();
}

int
WasapiBackend::set_midi_device_enabled (std::string const, bool)
{
	return 0;
}

bool
WasapiBackend::midi_device_enabled (std::string const) const
{
	return false;
}

int
WasapiBackend::_start (bool /*for_latency_measurement*/)
{
	if (_run) {
		return 0;
	}

	if (_io.open (_output_device, _input_device, _samplerate, _buffer_size) != 0) {
		error << "WASAPI: shared-mode open failed (stub or device error)" << endmsg;
		return AudioDeviceOpenError;
	}

	_samplerate = _io.sample_rate ();
	_buffer_size = _io.period_frames ();
	_run = true;
	/* process thread start: Windows agent completes wait_and_transfer loop */
	_active = false;
	return AudioDeviceOpenError; /* stub incomplete until COM path fills in */
}

int
WasapiBackend::stop ()
{
	_run = false;
	_io.close ();
	_active = false;
	return 0;
}

int
WasapiBackend::freewheel (bool start_stop)
{
	_freewheeling = start_stop;
	return 0;
}

float
WasapiBackend::dsp_load () const
{
	return 100.f * _dsp_load;
}

size_t
WasapiBackend::raw_buffer_size (DataType t)
{
	if (t == DataType::AUDIO) {
		return sizeof (Sample) * _buffer_size;
	}
	return 0;
}

samplepos_t
WasapiBackend::sample_time ()
{
	return 0;
}

samplepos_t
WasapiBackend::sample_time_at_cycle_start ()
{
	return 0;
}

pframes_t
WasapiBackend::samples_since_cycle_start ()
{
	return 0;
}

int
WasapiBackend::create_process_thread (std::function<void()> func)
{
	_pending_threads.push_back (func);
	return 0;
}

int
WasapiBackend::join_process_threads ()
{
	_pending_threads.clear ();
	return 0;
}

bool
WasapiBackend::in_process_thread ()
{
	return false;
}

uint32_t
WasapiBackend::process_thread_count ()
{
	return 0;
}

void
WasapiBackend::update_latencies ()
{
}

void*
WasapiBackend::private_handle () const
{
	return 0;
}

const std::string&
WasapiBackend::my_name () const
{
	return _instance_name;
}

BackendPort*
WasapiBackend::port_factory (std::string const& name, ARDOUR::DataType dt, ARDOUR::PortFlags flags)
{
	if (dt == DataType::AUDIO) {
		return new WasapiAudioPort (*this, name, flags);
	} else if (dt == DataType::MIDI) {
		return new WasapiMidiPort (*this, name, flags);
	}
	return 0;
}

PortEngine::PortPtr
WasapiBackend::register_port (const std::string& shortname, ARDOUR::DataType type, ARDOUR::PortFlags flags)
{
	return PortEngineSharedImpl::register_port (shortname, type, flags);
}

int
WasapiBackend::midi_event_get (pframes_t&, size_t&, uint8_t const**, void*, uint32_t)
{
	return -1;
}

int
WasapiBackend::midi_event_put (void*, pframes_t, const uint8_t*, size_t)
{
	return -1;
}

uint32_t
WasapiBackend::get_midi_event_count (void*)
{
	return 0;
}

void
WasapiBackend::midi_clear (void*)
{
}

bool
WasapiBackend::can_monitor_input () const
{
	return false;
}

int
WasapiBackend::request_input_monitoring (PortHandle, bool)
{
	return -1;
}

int
WasapiBackend::ensure_input_monitoring (PortHandle, bool)
{
	return -1;
}

bool
WasapiBackend::monitoring_input (PortHandle)
{
	return false;
}

void
WasapiBackend::set_latency_range (PortHandle port, bool for_playback, LatencyRange r)
{
	PortEngineSharedImpl::set_latency_range (port, for_playback, r);
}

LatencyRange
WasapiBackend::get_latency_range (PortHandle port, bool for_playback)
{
	return PortEngineSharedImpl::get_latency_range (port, for_playback);
}

void*
WasapiBackend::get_buffer (PortHandle port, pframes_t nframes)
{
	BackendPortPtr p = std::dynamic_pointer_cast<BackendPort> (port);
	if (!p) {
		return 0;
	}
	return p->get_buffer (nframes);
}

void*
WasapiBackend::process_thread (void*)
{
	return 0;
}

/* —— module descriptor —— */

static std::shared_ptr<WasapiBackend> _instance;

static std::shared_ptr<AudioBackend> backend_factory (AudioEngine& e);
static int instantiate (const std::string& arg1, const std::string& /*arg2*/);
static int deinstantiate ();
static bool already_configured ();
static bool available ();

static ARDOUR::AudioBackendInfo _descriptor = {
	BACKEND_NAME,
	instantiate,
	deinstantiate,
	backend_factory,
	already_configured,
	available
};

static std::shared_ptr<AudioBackend>
backend_factory (AudioEngine& e)
{
	if (!_instance) {
		_instance.reset (new WasapiBackend (e, _descriptor));
	}
	return _instance;
}

static int
instantiate (const std::string& arg1, const std::string& /*arg2*/)
{
	s_instance_name = arg1;
	return 0;
}

static int
deinstantiate ()
{
	_instance.reset ();
	return 0;
}

static bool
already_configured ()
{
	return false;
}

static bool
available ()
{
#ifdef PLATFORM_WINDOWS
	return true;
#else
	return false;
#endif
}

extern "C" ARDOURBACKEND_API ARDOUR::AudioBackendInfo*
descriptor ()
{
	return &_descriptor;
}

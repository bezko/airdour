/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <glib.h>
#include <glibmm/main.h>

#include "ardour/port_manager.h"

#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/i18n.h"
#include "pbd/pthread_utils.h"
#include "pbd/timing.h"

#ifdef _WIN32
#include "pbd/windows_mmcss.h"
#include "pbd/windows_timer_utils.h"
#endif

#include "ardour/audioengine.h"
#include "ardour/debug.h"

#include "audio_utils.h"
#include "debug.h"
#include "wasapi_backend.h"
#include "winmmemidi_output_device.h"

using namespace ARDOUR;
using namespace PBD;

#ifndef BACKEND_NAME
#define BACKEND_NAME "WASAPI"
#endif

static const char* const winmme_driver_name = X_("WinMME");
static const char* const none_midi_driver_name = X_("None");

static std::string s_instance_name;
static std::shared_ptr<WasapiBackend> _instance;

static uint32_t
elf_hash (std::string const& s)
{
	const uint8_t* b = (const uint8_t*)s.c_str ();
	uint32_t       h = 0;
	for (size_t i = 0; i < s.length (); ++i) {
		h             = (h << 4) + b[i];
		uint32_t high = h & 0xF0000000;
		if (high) {
			h ^= high >> 24;
			h &= ~high;
		}
	}
	return h;
}

/* —— ports —— */

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

WasapiMidiEvent::WasapiMidiEvent (const pframes_t timestamp, const uint8_t* data, size_t size)
	: _size (size > sizeof (_data) ? sizeof (_data) : size)
	, _timestamp (timestamp)
{
	if (_size && data) {
		memcpy (_data, data, _size);
	}
}

WasapiMidiEvent::WasapiMidiEvent (const WasapiMidiEvent& other)
	: _size (other._size)
	, _timestamp (other._timestamp)
{
	if (_size) {
		memcpy (_data, other._data, _size);
	}
}

WasapiMidiPort::WasapiMidiPort (WasapiBackend& b, const std::string& name, PortFlags flags)
	: BackendPort (b, name, flags)
	, _bufperiod (0)
	, _n_periods (1)
{
	_buffer[0].reserve (256);
	_buffer[1].reserve (256);
}

WasapiMidiPort::~WasapiMidiPort () {}

void*
WasapiMidiPort::get_buffer (pframes_t /*nframes*/)
{
	if (is_input ()) {
		_buffer[_bufperiod].clear ();
		const std::set<BackendPortPtr>& connections = get_connections ();
		for (std::set<BackendPortPtr>::const_iterator i = connections.begin ();
		     i != connections.end (); ++i) {
			std::shared_ptr<WasapiMidiPort> src = std::dynamic_pointer_cast<WasapiMidiPort> (*i);
			if (!src) {
				continue;
			}
			const WasapiMidiBuffer* sb = src->const_buffer ();
			for (WasapiMidiBuffer::const_iterator it = sb->begin (); it != sb->end (); ++it) {
				_buffer[_bufperiod].push_back (*it);
			}
		}
		std::stable_sort (_buffer[_bufperiod].begin (), _buffer[_bufperiod].end ());
	}
	return &_buffer[_bufperiod];
}

/* —— MMCSS —— */

#ifdef _WIN32
bool
WasapiBackend::set_mmcss_pro_audio (void** task_handle)
{
	HANDLE h = NULL;
	if (!PBD::MMCSS::initialize ()) {
		return false;
	}
	if (!PBD::MMCSS::set_thread_characteristics ("Pro Audio", &h)) {
		return false;
	}
	/* HIGH matches WinMME MIDI out; shared-mode callback needs headroom on laptops. */
	PBD::MMCSS::set_thread_priority (h, PBD::MMCSS::AVRT_PRIORITY_HIGH);
	*task_handle = (void*)h;
	return true;
}

bool
WasapiBackend::reset_mmcss (void* task_handle)
{
	if (!task_handle) {
		return false;
	}
	return PBD::MMCSS::revert_thread_characteristics ((HANDLE)task_handle);
}
#endif

/* —— ctor / dtor —— */

WasapiBackend::WasapiBackend (AudioEngine& e, AudioBackendInfo& info)
	: AudioBackend (e, info)
	, PortEngineSharedImpl (e, s_instance_name.empty () ? "ardour" : s_instance_name)
	, _instance_name (s_instance_name)
	, _input_device ("Default")
	, _output_device ("Default")
	, _samplerate (48000.f)
	, _buffer_size (480) /* ~10 ms @ 48 kHz shared — laptop product default */
	, _systemic_input_latency (0)
	, _systemic_output_latency (0)
	, _midi_driver_option (winmme_driver_name)
	, _midiio (0)
	, _run (false)
	, _active (false)
	, _freewheeling (false)
	, _freewheel (false)
	, _measure_latency (false)
	, _processed_samples (0)
	, _dsp_load (0.f)
	, _callback_last_us (0)
	, _callback_max_us (0)
	, _callback_sum_us (0)
	, _callback_count (0)
	, _xrun_count (0)
	, _main_thread_running (false)
{
#ifdef _WIN32
	_midiio = new WinMMEMidiIO ();
#endif
}

WasapiBackend::~WasapiBackend ()
{
	stop ();
	delete _midiio;
	_midiio = 0;
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

bool
WasapiBackend::update_devices ()
{
	std::vector<WasapiIO::DeviceInfo> devices;
	return _io.enumerate_devices (devices) == 0;
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
	r.push_back (DeviceStatus ("Default", true));
	r.push_back (DeviceStatus ("None", true));
	for (size_t i = 0; i < devices.size (); ++i) {
		if (devices[i].is_input) {
			r.push_back (DeviceStatus (devices[i].name, devices[i].available));
		}
	}
	return r;
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_output_devices () const
{
	std::vector<DeviceStatus> r;
	std::vector<WasapiIO::DeviceInfo> devices;
	const_cast<WasapiIO&> (_io).enumerate_devices (devices);
	r.push_back (DeviceStatus ("Default", true));
	for (size_t i = 0; i < devices.size (); ++i) {
		if (devices[i].is_output) {
			r.push_back (DeviceStatus (devices[i].name, devices[i].available));
		}
	}
	return r;
}

std::vector<float>
WasapiBackend::available_sample_rates (const std::string& /*device*/) const
{
	/*
	 * Shared mode locks to the endpoint mix format. Expose common rates for
	 * the UI; open() adopts the real mix rate and reports it back.
	 */
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
	std::vector<uint32_t> sizes;
	_io.recommended_buffer_sizes (sizes);
	if (sizes.empty ()) {
		sizes.push_back (256);
		sizes.push_back (512);
		sizes.push_back (1024);
		sizes.push_back (2048);
	}
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
	opts.push_back (none_midi_driver_name);
	opts.push_back (winmme_driver_name);
	return opts;
}

int
WasapiBackend::set_midi_option (const std::string& opt)
{
	if (opt != none_midi_driver_name && opt != winmme_driver_name) {
		return -1;
	}
	_midi_driver_option = opt;
	return 0;
}

std::string
WasapiBackend::midi_option () const
{
	return _midi_driver_option;
}

MidiDeviceInfo*
WasapiBackend::midi_device_info (const std::string& device_name) const
{
	if (!_midiio || _midi_driver_option != winmme_driver_name) {
		return 0;
	}
	std::vector<MidiDeviceInfo*> dev_info = _midiio->get_device_info ();
	for (std::vector<MidiDeviceInfo*>::const_iterator i = dev_info.begin ();
	     i != dev_info.end (); ++i) {
		if ((*i)->device_name == device_name) {
			return *i;
		}
	}
	return 0;
}

std::vector<AudioBackend::DeviceStatus>
WasapiBackend::enumerate_midi_devices () const
{
	std::vector<DeviceStatus> devices;
	if (!_midiio || _midi_driver_option != winmme_driver_name) {
		return devices;
	}
	const_cast<WinMMEMidiIO*> (_midiio)->update_device_info ();
	std::vector<MidiDeviceInfo*> info = _midiio->get_device_info ();
	for (std::vector<MidiDeviceInfo*>::const_iterator i = info.begin ();
	     i != info.end (); ++i) {
		devices.push_back (DeviceStatus ((*i)->device_name, true));
	}
	return devices;
}

int
WasapiBackend::set_midi_device_enabled (std::string const device, bool enable)
{
	MidiDeviceInfo* nfo = midi_device_info (device);
	if (!nfo) {
		return -1;
	}
	nfo->enable = enable;
	return 0;
}

bool
WasapiBackend::midi_device_enabled (std::string const device) const
{
	MidiDeviceInfo* nfo = midi_device_info (device);
	return nfo ? nfo->enable : false;
}

int
WasapiBackend::register_system_audio_ports ()
{
	LatencyRange lr;
	const uint32_t a_ins = _io.capture_channels ();
	const uint32_t a_out = _io.playback_channels ();

	lr.min = lr.max = (_measure_latency ? 0 : _systemic_input_latency);
	for (uint32_t i = 0; i < a_ins; ++i) {
		char tmp[64];
		snprintf (tmp, sizeof (tmp), "system:capture_%u", i + 1);
		PortPtr p = add_port (std::string (tmp), DataType::AUDIO,
		                      static_cast<PortFlags> (IsOutput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}
		set_latency_range (p, false, lr);
		_system_inputs.push_back (std::dynamic_pointer_cast<BackendPort> (p));
	}

	lr.min = lr.max = (_measure_latency ? 0 : _systemic_output_latency);
	for (uint32_t i = 0; i < a_out; ++i) {
		char tmp[64];
		snprintf (tmp, sizeof (tmp), "system:playback_%u", i + 1);
		PortPtr p = add_port (std::string (tmp), DataType::AUDIO,
		                      static_cast<PortFlags> (IsInput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}
		set_latency_range (p, true, lr);
		_system_outputs.push_back (std::dynamic_pointer_cast<BackendPort> (p));
	}
	return 0;
}

int
WasapiBackend::register_system_midi_ports (std::string const& device)
{
	if (!_midiio || _midi_driver_option != winmme_driver_name) {
		DEBUG_MIDI ("No WinMME MIDI backend selected\n");
		return 0;
	}

	LatencyRange lr;
	const std::vector<WinMMEMidiInputDevice*> inputs = _midiio->get_inputs ();

	for (std::vector<WinMMEMidiInputDevice*>::const_iterator i = inputs.begin ();
	     i != inputs.end (); ++i) {
		if (!device.empty () && device != (*i)->name ()) {
			continue;
		}
		MidiDeviceInfo* info = _midiio->get_device_info ((*i)->name ());
		if (!info || !info->enable) {
			continue;
		}

		char tmp[64];
		for (int x = 0; x < 10; ++x) {
			snprintf (tmp, sizeof (tmp), "system:midi_capture_%x%d", elf_hash ((*i)->name ()), x);
			if (!find_port (tmp)) {
				break;
			}
		}

		PortPtr p = add_port (std::string (tmp), DataType::MIDI,
		                      static_cast<PortFlags> (IsOutput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}

		lr.min = lr.max = (_measure_latency ? 0 : info->systemic_input_latency);
		set_latency_range (p, false, lr);

		BackendPortPtr ap = std::dynamic_pointer_cast<BackendPort> (p);
		ap->set_hw_port_name ((*i)->name ());

		std::shared_ptr<WasapiMidiPort> midi_port = std::dynamic_pointer_cast<WasapiMidiPort> (p);
		midi_clear (midi_port->get_buffer (0));
		_system_midi_in.push_back (midi_port);
		DEBUG_MIDI (string_compose ("Registered MIDI input port: %1 (%2)\n", tmp, (*i)->name ()));
	}

	const std::vector<WinMMEMidiOutputDevice*> outputs = _midiio->get_outputs ();

	for (std::vector<WinMMEMidiOutputDevice*>::const_iterator i = outputs.begin ();
	     i != outputs.end (); ++i) {
		if (!device.empty () && device != (*i)->name ()) {
			continue;
		}
		MidiDeviceInfo* info = _midiio->get_device_info ((*i)->name ());
		if (!info || !info->enable) {
			continue;
		}

		char tmp[64];
		for (int x = 0; x < 10; ++x) {
			snprintf (tmp, sizeof (tmp), "system:midi_playback_%x%d", elf_hash ((*i)->name ()), x);
			if (!find_port (tmp)) {
				break;
			}
		}
		PortPtr p = add_port (std::string (tmp), DataType::MIDI,
		                      static_cast<PortFlags> (IsInput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}

		lr.min = lr.max = (_measure_latency ? 0 : info->systemic_output_latency);
		set_latency_range (p, false, lr);

		BackendPortPtr ap = std::dynamic_pointer_cast<BackendPort> (p);
		ap->set_hw_port_name ((*i)->name ());

		std::shared_ptr<WasapiMidiPort> midi_port = std::dynamic_pointer_cast<WasapiMidiPort> (p);
		midi_port->set_n_periods (2);
		midi_clear (midi_port->get_buffer (0));
		_system_midi_out.push_back (midi_port);
		DEBUG_MIDI (string_compose ("Registered MIDI output port: %1 (%2)\n", tmp, (*i)->name ()));
	}

	return 0;
}

void
WasapiBackend::process_incoming_midi ()
{
	if (!_midiio || _midi_driver_option != winmme_driver_name) {
		return;
	}
	uint32_t i = 0;
	for (std::vector<BackendPortPtr>::const_iterator it = _system_midi_in.begin ();
	     it != _system_midi_in.end (); ++it, ++i) {
		WasapiMidiBuffer* mbuf = static_cast<WasapiMidiBuffer*> ((*it)->get_buffer (0));
		mbuf->clear ();
		uint64_t timestamp;
		uint8_t  data[MaxWinMidiEventSize];
		size_t   size = sizeof (data);
		while (_midiio->dequeue_input_event (
		           i,
		           _cycle_timer.get_start (),
		           _cycle_timer.get_next_start (),
		           timestamp,
		           data,
		           size)) {
			pframes_t sample_offset = _cycle_timer.samples_since_cycle_start (timestamp);
			midi_event_put (mbuf, sample_offset, data, size);
			size = sizeof (data);
		}
	}
}

void
WasapiBackend::process_outgoing_midi ()
{
	if (!_midiio || _midi_driver_option != winmme_driver_name) {
		return;
	}
	for (std::vector<BackendPortPtr>::iterator it = _system_midi_out.begin ();
	     it != _system_midi_out.end (); ++it) {
		std::dynamic_pointer_cast<WasapiMidiPort> (*it)->next_period ();
	}
	uint32_t i = 0;
	for (std::vector<BackendPortPtr>::const_iterator it = _system_midi_out.begin ();
	     it != _system_midi_out.end (); ++it, ++i) {
		const WasapiMidiBuffer* src =
			std::dynamic_pointer_cast<const WasapiMidiPort> (*it)->const_buffer ();
		for (WasapiMidiBuffer::const_iterator mit = src->begin (); mit != src->end (); ++mit) {
			uint64_t timestamp =
			    _cycle_timer.timestamp_from_sample_offset (mit->timestamp ());
			_midiio->enqueue_output_event (i, timestamp, mit->data (), mit->size ());
		}
	}
}

int
WasapiBackend::_start (bool for_latency_measurement)
{
#ifndef _WIN32
	(void)for_latency_measurement;
	error << _("WASAPI backend requires Windows") << endmsg;
	return BackendInitializationError;
#else
	if (_run) {
		return NoError;
	}

	_measure_latency = for_latency_measurement;

	if (_io.open (_output_device, _input_device, _samplerate, _buffer_size) != 0) {
		error << _("WASAPI: failed to open shared-mode device") << endmsg;
		return AudioDeviceOpenError;
	}

	_samplerate = _io.sample_rate ();
	_buffer_size = _io.period_frames ();

	_interleaved_capture.assign (_buffer_size * std::max (1u, _io.capture_channels ()), 0.f);
	_interleaved_playback.assign (_buffer_size * std::max (1u, _io.playback_channels ()), 0.f);

	clear_ports ();
	_system_midi_in.clear ();
	_system_midi_out.clear ();

	if (register_system_audio_ports ()) {
		_io.close ();
		return PortRegistrationError;
	}

	if (_midiio && _midi_driver_option == winmme_driver_name) {
		_midiio->start ();
		if (register_system_midi_ports ()) {
			_midiio->stop ();
			_io.close ();
			return PortRegistrationError;
		}
	}

	_cycle_timer.set_samplerate (_samplerate);
	_cycle_timer.set_samples_per_cycle (_buffer_size);

	engine.sample_rate_change (_samplerate);
	engine.buffer_size_change (_buffer_size);

	if (engine.reestablish_ports ()) {
		if (_midiio) {
			_midiio->stop ();
		}
		_io.close ();
		return PortReconnectError;
	}

	_run = true;
	_freewheel = false;
	_freewheeling = false;
	_processed_samples = 0;
	_port_change_flag.store (0);

	engine.reconnect_ports ();
	_dsp_load_calc.set_max_time (_samplerate, _buffer_size);
	_dsp_load_calc.reset ();

	PBD::MMTIMERS::set_min_resolution ();

	if (!start_main_process_thread ()) {
		_run = false;
		if (_midiio) {
			_midiio->stop ();
		}
		_io.close ();
		return ProcessThreadStartError;
	}

	/* Wait briefly for the thread to become active. */
	int timeout = 5000;
	while (!_active && --timeout > 0) {
		g_usleep (1000);
	}
	if (!_active) {
		error << _("WASAPI: process thread failed to start") << endmsg;
		stop ();
		return ProcessThreadStartError;
	}

	return NoError;
#endif
}

bool
WasapiBackend::start_main_process_thread ()
{
	if (pbd_realtime_pthread_create ("WASAPI Main", PBD_SCHED_FIFO, PBD_RT_PRI_MAIN,
	                                 PBD_RT_STACKSIZE_PROC, &_main_thread,
	                                 main_process_thread, this)) {
		if (pbd_pthread_create (PBD_RT_STACKSIZE_PROC, &_main_thread, main_process_thread, this)) {
			return false;
		}
	}
	_main_thread_running = true;
	return true;
}

bool
WasapiBackend::stop_main_process_thread ()
{
	if (_main_thread_running) {
		void* status = 0;
		pthread_join (_main_thread, &status);
		_main_thread_running = false;
	}
	return true;
}

void*
WasapiBackend::main_process_thread (void* arg)
{
	WasapiBackend* self = static_cast<WasapiBackend*> (arg);
	AudioEngine::thread_init_callback (self);

#ifdef _WIN32
	void* mmcss = 0;
	bool mmcss_ok = set_mmcss_pro_audio (&mmcss);
#endif

	self->_active = true;

	self->manager.registration_callback ();
	self->manager.graph_order_callback ();

	while (self->_run) {
		if (self->_freewheeling != self->_freewheel) {
			self->_freewheel = self->_freewheeling;
			self->engine.freewheel_callback (self->_freewheel);
			if (!self->_freewheel) {
				self->_dsp_load_calc.reset ();
			}
		}

		bool ok;
		if (!self->_freewheel) {
			ok = self->run_one_cycle ();
		} else {
			ok = self->run_freewheel_cycle ();
		}

		if (!ok) {
			break;
		}

		self->process_port_connection_changes ();
	}

	self->_active = false;
	if (self->_run) {
		self->engine.halted_callback (AudioBackend::get_error_string (AudioDeviceIOError).c_str ());
	}

#ifdef _WIN32
	if (mmcss_ok) {
		reset_mmcss (mmcss);
	}
#endif
	return 0;
}

bool
WasapiBackend::run_one_cycle ()
{
	/* Wait outside the DSP window — including WaitForSingleObject would make
	 * load look ~100% for every healthy shared-mode period. */
	int w = _io.wait_for_period ();
	if (w < 0) {
		return false;
	}
	if (w == 0) {
		return true; /* timeout — keep running */
	}

	const int64_t cycle_us = PBD::get_microseconds ();
	_cycle_timer.reset_start (cycle_us);
	_dsp_load_calc.set_start_timestamp_us (cycle_us);

	float* cap = _interleaved_capture.empty () ? 0 : &_interleaved_capture[0];
	float* play = _interleaved_playback.empty () ? 0 : &_interleaved_playback[0];

	if (_io.read_capture (cap, _buffer_size) < 0) {
		return false;
	}

	const uint32_t nframes = _buffer_size;
	const uint32_t n_in = _io.capture_channels ();
	const uint32_t n_out = _io.playback_channels ();

	process_incoming_midi ();

	uint32_t i = 0;
	for (std::vector<BackendPortPtr>::const_iterator it = _system_inputs.begin ();
	     it != _system_inputs.end (); ++it, ++i) {
		float* buf = (float*)(*it)->get_buffer (nframes);
		if (cap && i < n_in) {
			wasapi_deinterleave (cap, buf, nframes, i, n_in);
		} else {
			memset (buf, 0, nframes * sizeof (Sample));
		}
	}

	for (std::vector<BackendPortPtr>::const_iterator it = _system_outputs.begin ();
	     it != _system_outputs.end (); ++it) {
		memset ((*it)->get_buffer (nframes), 0, nframes * sizeof (Sample));
	}

	if (engine.process_callback (nframes)) {
		return false;
	}

	process_outgoing_midi ();

	/* Zero only this period's frames (not a larger scratch capacity). */
	if (play && n_out > 0) {
		memset (play, 0, (size_t)nframes * (size_t)n_out * sizeof (float));
	}
	i = 0;
	for (std::vector<BackendPortPtr>::const_iterator it = _system_outputs.begin ();
	     it != _system_outputs.end (); ++it, ++i) {
		float* buf = (float*)(*it)->get_buffer (nframes);
		if (play && i < n_out) {
			wasapi_interleave (buf, play, nframes, i, n_out);
		}
	}

	int wr = _io.write_playback (play, _buffer_size);
	if (wr == -2) {
		++_xrun_count;
		engine.Xrun ();
	} else if (wr < 0) {
		return false;
	}

	_processed_samples += nframes;
	const int64_t stop_us = PBD::get_microseconds ();
	_dsp_load_calc.set_stop_timestamp_us (stop_us);
	_dsp_load = _dsp_load_calc.get_dsp_load ();
	const int64_t dur = stop_us - cycle_us;
	if (dur >= 0) {
		_callback_last_us = dur;
		if ((uint64_t)dur > _callback_max_us) {
			_callback_max_us = (uint64_t)dur;
		}
		_callback_sum_us += (uint64_t)dur;
		++_callback_count;
	}
	return true;
}

bool
WasapiBackend::run_freewheel_cycle ()
{
	const uint32_t nframes = _buffer_size;
	for (std::vector<BackendPortPtr>::const_iterator it = _system_inputs.begin ();
	     it != _system_inputs.end (); ++it) {
		memset ((*it)->get_buffer (nframes), 0, nframes * sizeof (Sample));
	}
	if (engine.process_callback (nframes)) {
		return false;
	}
	_processed_samples += nframes;
	g_usleep ((gulong)(1e6 * (double)nframes / (double)_samplerate));
	return true;
}

void
WasapiBackend::process_port_connection_changes ()
{
	bool connections_changed = false;
	bool ports_changed = false;
	if (!pthread_mutex_trylock (&_port_callback_mutex)) {
		int canderef (1);
		if (_port_change_flag.compare_exchange_strong (canderef, 0)) {
			ports_changed = true;
		}
		if (!_port_connection_queue.empty ()) {
			connections_changed = true;
		}
		process_connection_queue_locked (manager);
		pthread_mutex_unlock (&_port_callback_mutex);
	}
	if (ports_changed) {
		manager.registration_callback ();
	}
	if (connections_changed) {
		manager.graph_order_callback ();
	}
	if (connections_changed || ports_changed) {
		update_system_port_latencies ();
		engine.latency_callback (false);
		engine.latency_callback (true);
	}
}

int
WasapiBackend::stop ()
{
	_run = false;
	stop_main_process_thread ();
	join_process_threads ();
	if (_midiio) {
		_midiio->stop ();
	}
	_system_midi_in.clear ();
	_system_midi_out.clear ();
	_io.close ();
	clear_ports ();
	_active = false;
	_interleaved_capture.clear ();
	_interleaved_playback.clear ();
#ifdef _WIN32
	PBD::MMTIMERS::reset_resolution ();
#endif
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

void
WasapiBackend::process_callback_stats (uint64_t& last_us, uint64_t& max_us,
                                       uint64_t& avg_us, uint64_t& count,
                                       uint64_t& xruns) const
{
	last_us = _callback_last_us;
	max_us = _callback_max_us;
	count = _callback_count;
	avg_us = (count > 0) ? (_callback_sum_us / count) : 0;
	xruns = _xrun_count;
}

size_t
WasapiBackend::raw_buffer_size (DataType t)
{
	/* MIDI size must be non-zero: ThreadBuffers always allocates ≥1 MIDI
	 * buffer via this value. Capacity 0 + WINDOWS_VST_SUPPORT makes
	 * BufferSet::VSTBuffer throw failed_constructor during Session
	 * PortCountChanged → ensure_buffers (Master ensure_io). Match Dummy/PA/ALSA. */
	static const size_t max_midi_buffer_size = 8192;
	switch (t) {
	case DataType::AUDIO:
		return sizeof (Sample) * _buffer_size;
	case DataType::MIDI:
		return max_midi_buffer_size;
	default:
		return 0;
	}
}

samplepos_t
WasapiBackend::sample_time ()
{
	return _processed_samples;
}

samplepos_t
WasapiBackend::sample_time_at_cycle_start ()
{
	return _processed_samples;
}

pframes_t
WasapiBackend::samples_since_cycle_start ()
{
	return 0;
}

void*
WasapiBackend::pthread_process (void* arg)
{
	ThreadData* td = static_cast<ThreadData*> (arg);
	std::function<void()> f = td->f;
#ifdef _WIN32
	void* mmcss = 0;
	bool ok = set_mmcss_pro_audio (&mmcss);
#endif
	f ();
#ifdef _WIN32
	if (ok) {
		reset_mmcss (mmcss);
	}
#endif
	delete td;
	return 0;
}

int
WasapiBackend::create_process_thread (std::function<void()> func)
{
	pthread_t id;
	ThreadData* td = new ThreadData (this, func);
	if (pbd_realtime_pthread_create ("WASAPI Proc", PBD_SCHED_FIFO, PBD_RT_PRI_PROC,
	                                 PBD_RT_STACKSIZE_PROC, &id, pthread_process, td)) {
		if (pbd_pthread_create (PBD_RT_STACKSIZE_PROC, &id, pthread_process, td)) {
			delete td;
			return -1;
		}
	}
	_threads.push_back (id);
	return 0;
}

int
WasapiBackend::join_process_threads ()
{
	int rv = 0;
	for (size_t i = 0; i < _threads.size (); ++i) {
		void* status = 0;
		if (pthread_join (_threads[i], &status)) {
			rv -= 1;
		}
	}
	_threads.clear ();
	return rv;
}

bool
WasapiBackend::in_process_thread ()
{
	if (_main_thread_running && pthread_equal (_main_thread, pthread_self ())) {
		return true;
	}
	for (size_t i = 0; i < _threads.size (); ++i) {
		if (pthread_equal (_threads[i], pthread_self ())) {
			return true;
		}
	}
	return false;
}

uint32_t
WasapiBackend::process_thread_count ()
{
	return (uint32_t)_threads.size ();
}

void
WasapiBackend::update_latencies ()
{
	port_connect_add_remove_callback ();
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
	}
	if (dt == DataType::MIDI) {
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
WasapiBackend::midi_event_get (pframes_t& timestamp, size_t& size, uint8_t const** buf, void* port_buffer, uint32_t event_index)
{
	if (!port_buffer) {
		return -1;
	}
	WasapiMidiBuffer& src = *static_cast<WasapiMidiBuffer*> (port_buffer);
	if (event_index >= src.size ()) {
		return -1;
	}
	WasapiMidiEvent const& ev = src[event_index];
	timestamp = ev.timestamp ();
	size = ev.size ();
	*buf = ev.data ();
	return 0;
}

int
WasapiBackend::midi_event_put (void* port_buffer, pframes_t timestamp, const uint8_t* buffer, size_t size)
{
	if (!port_buffer || !buffer || size == 0) {
		return -1;
	}
	WasapiMidiBuffer& dst = *static_cast<WasapiMidiBuffer*> (port_buffer);
	dst.push_back (WasapiMidiEvent (timestamp, buffer, size));
	return 0;
}

uint32_t
WasapiBackend::get_midi_event_count (void* port_buffer)
{
	if (!port_buffer) {
		return 0;
	}
	return (uint32_t)static_cast<WasapiMidiBuffer*> (port_buffer)->size ();
}

void
WasapiBackend::midi_clear (void* port_buffer)
{
	if (!port_buffer) {
		return;
	}
	static_cast<WasapiMidiBuffer*> (port_buffer)->clear ();
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
	BackendPortPtr p = std::dynamic_pointer_cast<BackendPort> (port);
	if (!valid_port (p)) {
		return;
	}
	p->set_latency_range (r, for_playback);
}

LatencyRange
WasapiBackend::get_latency_range (PortHandle port, bool for_playback)
{
	LatencyRange r;
	BackendPortPtr p = std::dynamic_pointer_cast<BackendPort> (port);
	if (!valid_port (p)) {
		r.min = 0;
		r.max = 0;
		return r;
	}

	r = p->latency_range (for_playback);
	if (p->is_physical () && p->is_terminal ()) {
		/* Shared-mode device latency is at least one period until
		 * WasapiIO exposes measured capture/playback latencies. */
		if (p->is_input () && for_playback) {
			r.min += _buffer_size;
			r.max += _buffer_size;
		}
		if (p->is_output () && !for_playback) {
			r.min += _buffer_size;
			r.max += _buffer_size;
		}
	}
	return r;
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

/* —— module descriptor —— */

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
#ifdef _WIN32
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

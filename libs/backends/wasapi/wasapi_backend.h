/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pthread.h>

#include "ardour/audio_backend.h"
#include "ardour/dsp_load_calculator.h"
#include "ardour/port_engine_shared.h"
#include "ardour/types.h"

#include "wasapi_io.h"

namespace ARDOUR {

class WasapiBackend;

class WasapiAudioPort : public BackendPort
{
public:
	WasapiAudioPort (WasapiBackend& b, const std::string&, PortFlags);
	~WasapiAudioPort ();

	DataType type () const { return DataType::AUDIO; }

	Sample*       buffer () { return _buffer; }
	const Sample* const_buffer () const { return _buffer; }
	void*         get_buffer (pframes_t nframes);

private:
	Sample _buffer[8192];
};

class WasapiMidiPort : public BackendPort
{
public:
	WasapiMidiPort (WasapiBackend& b, const std::string&, PortFlags);
	~WasapiMidiPort ();

	DataType type () const { return DataType::MIDI; }

	void* get_buffer (pframes_t nframes);

private:
	uint8_t _placeholder[1];
};

/**
 * Native WASAPI shared-mode backend (AUDCLNT_SHAREMODE_SHARED).
 * Exclusive / DirectSound / ASIO are out of scope.
 */
class WasapiBackend : public AudioBackend, public PortEngineSharedImpl
{
public:
	WasapiBackend (AudioEngine& e, AudioBackendInfo& info);
	~WasapiBackend ();

	std::string name () const;
	bool        is_realtime () const;

	bool use_separate_input_and_output_devices () const { return true; }
	bool can_request_update_devices () { return true; }
	bool update_devices ();

	std::vector<DeviceStatus> enumerate_devices () const;
	std::vector<DeviceStatus> enumerate_input_devices () const;
	std::vector<DeviceStatus> enumerate_output_devices () const;

	std::vector<float>    available_sample_rates (const std::string& device) const;
	std::vector<uint32_t> available_buffer_sizes (const std::string& device) const;

	bool can_change_sample_rate_when_running () const { return false; }
	bool can_change_buffer_size_when_running () const { return false; }
	bool can_measure_systemic_latency () const { return true; }

	int set_device_name (const std::string&);
	int set_input_device_name (const std::string&);
	int set_output_device_name (const std::string&);
	int set_sample_rate (float);
	int set_buffer_size (uint32_t);
	int set_interleaved (bool yn);
	int set_systemic_input_latency (uint32_t);
	int set_systemic_output_latency (uint32_t);
	int set_systemic_midi_input_latency (std::string const, uint32_t);
	int set_systemic_midi_output_latency (std::string const, uint32_t);

	int reset_device () { return 0; }

	std::string device_name () const;
	std::string input_device_name () const;
	std::string output_device_name () const;
	float       sample_rate () const;
	uint32_t    buffer_size () const;
	bool        interleaved () const;
	uint32_t    systemic_input_latency () const;
	uint32_t    systemic_output_latency () const;
	uint32_t    systemic_midi_input_latency (std::string const) const;
	uint32_t    systemic_midi_output_latency (std::string const) const;

	std::string control_app_name () const;
	void        launch_control_app ();

	std::vector<std::string> enumerate_midi_options () const;
	int                      set_midi_option (const std::string&);
	std::string              midi_option () const;

	std::vector<DeviceStatus> enumerate_midi_devices () const;
	int                       set_midi_device_enabled (std::string const, bool);
	bool                      midi_device_enabled (std::string const) const;
	bool                      can_set_systemic_midi_latencies () const { return true; }

protected:
	int _start (bool for_latency_measurement);

public:
	int    stop ();
	int    freewheel (bool);
	float  dsp_load () const;
	size_t raw_buffer_size (DataType t);

	samplepos_t sample_time ();
	samplepos_t sample_time_at_cycle_start ();
	pframes_t   samples_since_cycle_start ();

	int      create_process_thread (std::function<void()> func);
	int      join_process_threads ();
	bool     in_process_thread ();
	uint32_t process_thread_count ();

	void update_latencies ();

	void*              private_handle () const;
	const std::string& my_name () const;

	bool                port_is_physical (PortEngine::PortHandle ph) const { return PortEngineSharedImpl::port_is_physical (ph); }
	void                get_physical_outputs (DataType type, std::vector<std::string>& results) { PortEngineSharedImpl::get_physical_outputs (type, results); }
	void                get_physical_inputs (DataType type, std::vector<std::string>& results) { PortEngineSharedImpl::get_physical_inputs (type, results); }
	ChanCount           n_physical_outputs () const { return PortEngineSharedImpl::n_physical_outputs (); }
	ChanCount           n_physical_inputs () const { return PortEngineSharedImpl::n_physical_inputs (); }
	uint32_t            port_name_size () const { return PortEngineSharedImpl::port_name_size (); }
	int                 set_port_name (PortEngine::PortHandle ph, const std::string& name) { return PortEngineSharedImpl::set_port_name (ph, name); }
	std::string         get_port_name (PortEngine::PortHandle ph) const { return PortEngineSharedImpl::get_port_name (ph); }
	PortFlags           get_port_flags (PortEngine::PortHandle ph) const { return PortEngineSharedImpl::get_port_flags (ph); }
	PortEngine::PortPtr get_port_by_name (std::string const& name) const { return PortEngineSharedImpl::get_port_by_name (name); }
	int                 get_ports (const std::string& port_name_pattern, DataType type, PortFlags flags, std::vector<std::string>& results) const { return PortEngineSharedImpl::get_ports (port_name_pattern, type, flags, results); }
	DataType            port_data_type (PortEngine::PortHandle ph) const { return PortEngineSharedImpl::port_data_type (ph); }
	PortEngine::PortPtr register_port (const std::string& shortname, ARDOUR::DataType type, ARDOUR::PortFlags flags);
	void                unregister_port (PortHandle ph) { if (!_run) return; PortEngineSharedImpl::unregister_port (ph); }
	int                 connect (const std::string& src, const std::string& dst) { return PortEngineSharedImpl::connect (src, dst); }
	int                 disconnect (const std::string& src, const std::string& dst) { return PortEngineSharedImpl::disconnect (src, dst); }
	int                 connect (PortEngine::PortHandle ph, const std::string& other) { return PortEngineSharedImpl::connect (ph, other); }
	int                 disconnect (PortEngine::PortHandle ph, const std::string& other) { return PortEngineSharedImpl::disconnect (ph, other); }
	int                 disconnect_all (PortEngine::PortHandle ph) { return PortEngineSharedImpl::disconnect_all (ph); }
	bool                connected (PortEngine::PortHandle ph, bool process_callback_safe) { return PortEngineSharedImpl::connected (ph, process_callback_safe); }
	bool                connected_to (PortEngine::PortHandle ph, const std::string& other, bool process_callback_safe) { return PortEngineSharedImpl::connected_to (ph, other, process_callback_safe); }
	bool                physically_connected (PortEngine::PortHandle ph, bool process_callback_safe) { return PortEngineSharedImpl::physically_connected (ph, process_callback_safe); }
	int                 get_connections (PortEngine::PortHandle ph, std::vector<std::string>& results, bool process_callback_safe) { return PortEngineSharedImpl::get_connections (ph, results, process_callback_safe); }

	int      midi_event_get (pframes_t& timestamp, size_t& size, uint8_t const** buf, void* port_buffer, uint32_t event_index);
	int      midi_event_put (void* port_buffer, pframes_t timestamp, const uint8_t* buffer, size_t size);
	uint32_t get_midi_event_count (void* port_buffer);
	void     midi_clear (void* port_buffer);

	bool can_monitor_input () const;
	int  request_input_monitoring (PortHandle, bool);
	int  ensure_input_monitoring (PortHandle, bool);
	bool monitoring_input (PortHandle);

	void         set_latency_range (PortHandle, bool for_playback, LatencyRange);
	LatencyRange get_latency_range (PortHandle, bool for_playback);

	void* get_buffer (PortHandle, pframes_t);

	BackendPort* port_factory (std::string const& name, ARDOUR::DataType dt, ARDOUR::PortFlags flags);

private:
	struct ThreadData {
		WasapiBackend*          backend;
		std::function<void()>   f;
		ThreadData (WasapiBackend* b, std::function<void()> fn)
			: backend (b), f (fn) {}
	};

	static void* pthread_process (void*);
	static void* main_process_thread (void*);

	int  register_system_audio_ports ();
	bool start_main_process_thread ();
	bool stop_main_process_thread ();
	bool run_one_cycle ();
	bool run_freewheel_cycle ();
	void process_port_connection_changes ();

#ifdef _WIN32
	static bool set_mmcss_pro_audio (void** task_handle);
	static bool reset_mmcss (void* task_handle);
#endif

	std::string _instance_name;
	WasapiIO    _io;

	std::string _input_device;
	std::string _output_device;
	float       _samplerate;
	uint32_t    _buffer_size;
	uint32_t    _systemic_input_latency;
	uint32_t    _systemic_output_latency;

	std::atomic<bool> _run;
	std::atomic<bool> _active;
	std::atomic<bool> _freewheeling;
	bool              _freewheel;
	bool              _measure_latency;

	samplepos_t _processed_samples;

	float             _dsp_load;
	DSPLoadCalculator _dsp_load_calc;

	pthread_t              _main_thread;
	bool                   _main_thread_running;
	std::vector<pthread_t> _threads;

	std::vector<float> _interleaved_capture;
	std::vector<float> _interleaved_playback;
};

} // namespace ARDOUR

/*
 * Copyright (C) 2026 Airdour contributors
 */

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "vst2_bridge_processor.h"
#include "vst2_bridge_client.h"
#include "vst2_bridge_protocol.h"

#include <algorithm>
#include <cstring>

#include "ardour/audio_buffer.h"
#include "ardour/buffer_set.h"
#include "ardour/midi_buffer.h"
#include "ardour/session.h"
#include "pbd/xml++.h"

namespace airdour {
namespace vst2_bridge {

Vst2BridgeProcessor::Vst2BridgeProcessor (ARDOUR::Session& s,
					  Temporal::TimeDomainProvider const& tdp,
					  const std::string& display_name,
					  const std::string& plugin_dll_path,
					  bool as_instrument)
	: ARDOUR::Processor (s, display_name, tdp)
	, _display_name (display_name)
	, _plugin_path (plugin_dll_path)
	, _bridged_ok (false)
	, _as_instrument (as_instrument)
	, _sample_rate ((float)s.nominal_sample_rate ())
	, _block_size (s.get_block_size ())
	, _param_count (0)
	, _n_inputs (as_instrument ? 0 : 2)
	, _n_outputs (2)
{
	_bridged_ok = ensure_bridge_running (_plugin_path);
	if (_bridged_ok) {
		uint32_t ni = 2, no = 2, np = 0;
		if (get_io_counts (_plugin_path, &ni, &no, &np)) {
			if (_as_instrument) {
				_n_inputs = (int)ni; /* often 0 for synths */
			} else {
				_n_inputs = ni > 0 ? (int)ni : 2;
			}
			_n_outputs = no > 0 ? (int)no : 2;
			_param_count = (int)std::min<uint32_t> (np, 12u);
			if (!_as_instrument && ni == 0 && no > 0) {
				/* PE said instrument-like; treat as synth. */
				_as_instrument = true;
				_n_inputs = 0;
			}
		}
		(void)set_sample_rate (_plugin_path, _sample_rate);
		(void)set_block_size (_plugin_path, (uint32_t)_block_size);
	}
	set_display_to_user (true);
}

Vst2BridgeProcessor::~Vst2BridgeProcessor ()
{
	shutdown_plugin (_plugin_path);
}

bool
Vst2BridgeProcessor::can_support_io_configuration (const ARDOUR::ChanCount& in,
						   ARDOUR::ChanCount& out)
{
	if (_as_instrument) {
		if (in.n_midi () == 0) {
			return false;
		}
		out = in;
		uint32_t n_audio_out = (uint32_t)std::max (2, _n_outputs);
		out.set (ARDOUR::DataType::AUDIO, n_audio_out);
		if (out.n_midi () == 0) {
			out.set (ARDOUR::DataType::MIDI, 1);
		}
		return true;
	}
	out = in;
	return in.n_audio () > 0;
}

bool
Vst2BridgeProcessor::configure_io (ARDOUR::ChanCount in, ARDOUR::ChanCount out)
{
	if (!_as_instrument && out != in) {
		return false;
	}
	return ARDOUR::Processor::configure_io (in, out);
}

int
Vst2BridgeProcessor::set_block_size (pframes_t nframes)
{
	_block_size = nframes;
	if (_bridged_ok) {
		(void)airdour::vst2_bridge::set_block_size (_plugin_path, (uint32_t)nframes);
	}
	return 0;
}

void
Vst2BridgeProcessor::run (ARDOUR::BufferSet& bufs, samplepos_t /*start_sample*/,
			  samplepos_t /*end_sample*/, double /*speed*/, pframes_t nframes,
			  bool result_required)
{
	if (!check_active () || !_bridged_ok || !result_required || nframes == 0) {
		return;
	}

	pframes_t frames = nframes;
	if (frames > airdour::vst2_bridge::kMaxBlock) {
		frames = airdour::vst2_bridge::kMaxBlock;
	}

	MidiEventWire midi_wire[airdour::vst2_bridge::kMaxMidiEvents];
	uint32_t midi_count = 0;
	if (bufs.count ().n_midi () > 0) {
		ARDOUR::MidiBuffer& mb = bufs.get_midi (0);
		for (ARDOUR::MidiBuffer::iterator it = mb.begin (); it != mb.end (); ++it) {
			Evoral::Event<ARDOUR::MidiBuffer::TimeType> ev (*it);
			if (ev.size () == 0 || ev.size () > 3) {
				continue;
			}
			if (midi_count >= airdour::vst2_bridge::kMaxMidiEvents) {
				break;
			}
			MidiEventWire& w = midi_wire[midi_count++];
			w.sample_offset = (uint32_t)std::min<samplepos_t> (ev.time (), frames > 0 ? frames - 1 : 0);
			w.size = (uint8_t)ev.size ();
			w.pad = 0;
			w.data[0] = w.data[1] = w.data[2] = 0;
			for (uint8_t b = 0; b < w.size; ++b) {
				w.data[b] = ev.buffer ()[b];
			}
		}
	}

	float* in_ptrs[airdour::vst2_bridge::kMaxChannels];
	float* out_ptrs[airdour::vst2_bridge::kMaxChannels];
	float scratch_out[airdour::vst2_bridge::kMaxChannels][airdour::vst2_bridge::kMaxBlock];
	float silence[airdour::vst2_bridge::kMaxBlock];
	std::memset (silence, 0, sizeof (silence));

	for (uint32_t c = 0; c < airdour::vst2_bridge::kMaxChannels; ++c) {
		in_ptrs[c] = silence;
		out_ptrs[c] = scratch_out[c];
	}

	const uint32_t n_audio = bufs.count ().n_audio ();
	uint32_t n_in = 0;
	uint32_t n_out = (uint32_t)std::max (1, _n_outputs);
	n_out = std::min (n_out, airdour::vst2_bridge::kMaxChannels);

	if (!_as_instrument) {
		if (n_audio == 0) {
			return;
		}
		n_in = std::min<uint32_t> (n_audio, (uint32_t)std::max (1, _n_inputs));
		n_in = std::min (n_in, airdour::vst2_bridge::kMaxChannels);
		n_out = std::min<uint32_t> (n_audio, (uint32_t)std::max (1, _n_outputs));
		n_out = std::min (n_out, airdour::vst2_bridge::kMaxChannels);
		for (uint32_t c = 0; c < n_in; ++c) {
			ARDOUR::AudioBuffer& ab = bufs.get_audio (c);
			in_ptrs[c] = ab.data (0);
		}
	} else {
		/* Instrument: silent audio in; write into available audio outs. */
		n_in = (uint32_t)std::max (0, _n_inputs);
		n_in = std::min (n_in, airdour::vst2_bridge::kMaxChannels);
		if (n_audio == 0) {
			/* Processor configured audio outs should exist; bail quietly. */
			return;
		}
		n_out = std::min<uint32_t> (std::max (n_audio, (uint32_t)std::max (1, _n_outputs)),
					    airdour::vst2_bridge::kMaxChannels);
	}

	if (!process_audio (_plugin_path, in_ptrs, out_ptrs, n_in, n_out, (uint32_t)frames,
			    _sample_rate, 50, midi_count > 0 ? midi_wire : nullptr, midi_count)) {
		/* Leave dry audio on timeout / IPC failure. */
		return;
	}

	uint32_t write_ch = n_audio > 0 ? n_audio : n_out;
	write_ch = std::min (write_ch, airdour::vst2_bridge::kMaxChannels);
	for (uint32_t c = 0; c < write_ch && c < n_audio; ++c) {
		ARDOUR::AudioBuffer& ab = bufs.get_audio (c);
		uint32_t src = c;
		if (src >= n_out) {
			src = n_out - 1;
		}
		std::memcpy (ab.data (0), scratch_out[src], frames * sizeof (float));
		if (frames < nframes) {
			ab.silence (nframes - frames, frames);
		}
	}
}

float
Vst2BridgeProcessor::get_param_norm (int index) const
{
	if (!_bridged_ok || index < 0 || index >= _param_count) {
		return 0.f;
	}
	float v = 0.f;
	if (!get_parameter (_plugin_path, (uint32_t)index, &v)) {
		return 0.f;
	}
	return std::max (0.f, std::min (1.f, v));
}

bool
Vst2BridgeProcessor::set_param_norm (int index, float norm_0_1)
{
	if (!_bridged_ok || index < 0 || index >= _param_count) {
		return false;
	}
	norm_0_1 = std::max (0.f, std::min (1.f, norm_0_1));
	return set_parameter (_plugin_path, (uint32_t)index, norm_0_1);
}

XMLNode&
Vst2BridgeProcessor::state () const
{
	XMLNode& node (ARDOUR::Processor::state ());
	node.set_property ("type", "vst2-bridge-x86");
	node.set_property ("plugin-path", _plugin_path);
	node.set_property ("instrument", _as_instrument ? 1 : 0);
	return node;
}

} /* namespace vst2_bridge */
} /* namespace airdour */

#endif /* AIRDOUR_WIN_SHELL_WITH_ENGINE */

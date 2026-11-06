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
#include "ardour/session.h"
#include "pbd/xml++.h"

namespace airdour {
namespace vst2_bridge {

Vst2BridgeProcessor::Vst2BridgeProcessor (ARDOUR::Session& s,
					  Temporal::TimeDomainProvider const& tdp,
					  const std::string& display_name,
					  const std::string& plugin_dll_path)
	: ARDOUR::Processor (s, display_name, tdp)
	, _display_name (display_name)
	, _plugin_path (plugin_dll_path)
	, _bridged_ok (false)
	, _configured_rate (false)
	, _sample_rate ((float)s.nominal_sample_rate ())
	, _block_size (s.get_block_size ())
	, _param_count (0)
	, _n_inputs (2)
	, _n_outputs (2)
{
	_bridged_ok = ensure_bridge_running (_plugin_path);
	if (_bridged_ok) {
		uint32_t ni = 2, no = 2, np = 0;
		if (get_io_counts (_plugin_path, &ni, &no, &np)) {
			_n_inputs = ni > 0 ? (int)ni : 2;
			_n_outputs = no > 0 ? (int)no : 2;
			_param_count = (int)std::min<uint32_t> (np, 12u);
		}
		(void)set_sample_rate (_plugin_path, _sample_rate);
		(void)set_block_size (_plugin_path, (uint32_t)_block_size);
		_configured_rate = true;
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
	out = in;
	return in.n_audio () > 0;
}

bool
Vst2BridgeProcessor::configure_io (ARDOUR::ChanCount in, ARDOUR::ChanCount out)
{
	if (out != in) {
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

	const uint32_t n_audio = bufs.count ().n_audio ();
	if (n_audio == 0) {
		return;
	}

	uint32_t n_in = std::min<uint32_t> (n_audio, (uint32_t)_n_inputs);
	uint32_t n_out = std::min<uint32_t> (n_audio, (uint32_t)_n_outputs);
	if (n_in == 0) {
		n_in = 1;
	}
	if (n_out == 0) {
		n_out = 1;
	}
	n_in = std::min (n_in, airdour::vst2_bridge::kMaxChannels);
	n_out = std::min (n_out, airdour::vst2_bridge::kMaxChannels);

	float* in_ptrs[airdour::vst2_bridge::kMaxChannels];
	float* out_ptrs[airdour::vst2_bridge::kMaxChannels];
	float scratch_out[airdour::vst2_bridge::kMaxChannels][airdour::vst2_bridge::kMaxBlock];

	pframes_t frames = nframes;
	if (frames > airdour::vst2_bridge::kMaxBlock) {
		frames = airdour::vst2_bridge::kMaxBlock;
	}

	for (uint32_t c = 0; c < airdour::vst2_bridge::kMaxChannels; ++c) {
		in_ptrs[c] = nullptr;
		out_ptrs[c] = scratch_out[c];
	}
	for (uint32_t c = 0; c < n_in; ++c) {
		ARDOUR::AudioBuffer& ab = bufs.get_audio (c);
		in_ptrs[c] = ab.data (0);
	}
	/* Mono plugin on stereo track: duplicate input. */
	if (n_in == 1 && n_audio > 1) {
		/* still feed channel 0 only */
	}

	if (!process_audio (_plugin_path, in_ptrs, out_ptrs, n_in, n_out, (uint32_t)frames,
			    _sample_rate)) {
		/* Leave dry audio on timeout / IPC failure. */
		return;
	}

	for (uint32_t c = 0; c < n_audio; ++c) {
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
	return node;
}

} /* namespace vst2_bridge */
} /* namespace airdour */

#endif /* AIRDOUR_WIN_SHELL_WITH_ENGINE */

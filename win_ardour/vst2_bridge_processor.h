/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Session Processor that hosts a 32-bit VST2 via ardour-vst2-bridge-x86.exe.
 * Copies audio (+ optional MIDI) to SHM, runs Op::Process, writes audio result.
 */
#pragma once

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include <string>

#include "ardour/processor.h"
#include "ardour/types.h"
#include "temporal/domain_provider.h"

namespace airdour {
namespace vst2_bridge {

class Vst2BridgeProcessor : public ARDOUR::Processor
{
public:
	Vst2BridgeProcessor (ARDOUR::Session& s,
			     Temporal::TimeDomainProvider const& tdp,
			     const std::string& display_name,
			     const std::string& plugin_dll_path,
			     bool as_instrument = false);
	~Vst2BridgeProcessor ();

	std::string display_name () const override { return _display_name; }

	bool can_support_io_configuration (const ARDOUR::ChanCount& in,
					   ARDOUR::ChanCount& out) override;
	bool configure_io (ARDOUR::ChanCount in, ARDOUR::ChanCount out) override;

	void run (ARDOUR::BufferSet& bufs, samplepos_t start_sample, samplepos_t end_sample,
		  double speed, pframes_t nframes, bool result_required) override;

	int set_block_size (pframes_t nframes) override;

	bool bridged_ok () const { return _bridged_ok; }
	bool is_instrument () const { return _as_instrument; }
	const std::string& plugin_path () const { return _plugin_path; }

	int param_count () const { return _param_count; }
	float get_param_norm (int index) const;
	bool set_param_norm (int index, float norm_0_1);

protected:
	XMLNode& state () const override;

private:
	std::string _display_name;
	std::string _plugin_path;
	bool _bridged_ok;
	bool _as_instrument;
	bool _configured_rate;
	float _sample_rate;
	pframes_t _block_size;
	int _param_count;
	int _n_inputs;
	int _n_outputs;
};

} /* namespace vst2_bridge */
} /* namespace airdour */

#endif /* AIRDOUR_WIN_SHELL_WITH_ENGINE */

// Proof tool (read-only): maps every stream output of an aitum.json onto our model and prints the
// result. Never prints a key or a server address, only "present"/"absent". Usage: oms_aitum_probe <aitum.json>
#include "../../../src/core/aitum.hpp"
#include "../../../src/core/config.hpp"

#include <cstdio>

using namespace oms;

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::printf("usage: oms_aitum_probe <aitum.json>\n");
		return 2;
	}
	std::string text;
	if (!read_file(argv[1], text)) {
		std::printf("cannot read the file\n");
		return 1;
	}
	AitumConfig c = parse_aitum(text);
	std::printf("status: %d (0=Ok 1=NotFound 2=TooBig 3=Malformed 4=UnknownShape) %s\n", static_cast<int>(c.status), c.message.c_str());
	for (size_t i = 0; i < c.canvases.size(); ++i)
		std::printf("canvas %zu: \"%s\" %dx%d\n", i, c.canvases[i].name.c_str(), c.canvases[i].width, c.canvases[i].height);
	std::printf("stream outputs: %zu, other entries skipped: %d\n", c.outputs.size(), c.skipped_outputs);
	for (size_t i = 0; i < c.outputs.size(); ++i) {
		const AitumOutput &o = c.outputs[i];
		Destination base;
		base.id = "probe";
		base.name = "probe";
		base.platform = "custom";
		base.url = "rtmp://placeholder.invalid/live";
		Destination d;
		std::string key;
		std::vector<std::string> notes;
		bool ok = apply_aitum(base, o, d, key, notes);
		std::printf("--- output \"%s\" enabled=%d server=%s key=%s\n", o.name.c_str(), o.enabled ? 1 : 0, o.server.empty() ? "absent" : "present",
			    key.empty() ? "absent" : "present");
		std::printf("    maps=%s canvas=\"%s\" encoder=%s bitrate=%d keyint=%d preset=%s profile=%s rate=%s rescale=%dx%d scale_type=%d\n",
			    ok ? "yes" : "NO", d.canvas.c_str(), d.video_encoder.c_str(), d.bitrate_kbps, d.keyint_sec, d.preset.c_str(),
			    d.profile.c_str(), d.rate_control.c_str(), d.out_width, d.out_height, d.scale_type);
		std::printf("    audio_track=%d audio_bitrate=%d delay=%d extra=%zu settings\n", d.audio_track, d.audio_bitrate_kbps, d.delay_sec,
			    d.venc_extra.size());
		for (size_t k = 0; k < notes.size(); ++k)
			std::printf("    note: %s\n", notes[k].c_str());
		secure_zero(key);
	}
	wipe(c);
	return 0;
}

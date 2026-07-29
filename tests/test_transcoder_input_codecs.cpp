#include "mfx50_transcoder.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static int require_true(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "require failed: %s\n", message);
    return 1;
}

int main() {
#ifdef _WIN32
    return 0;
#else
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("mfx50_transcoder_input_codecs_" + std::to_string(stamp));
    fs::create_directories(root);

    fs::path fake_sample = root / "fake_sample_multi_transcode.sh";
    {
        std::ofstream script(fake_sample);
        script << "#!/bin/sh\n";
        script << "echo 'Common transcoding time is 1.000 sec'\n";
        script << "i=0\n";
        script << "while [ \"$i\" -lt 5 ]; do\n";
        script << "  echo \"*** session $i PASSED 1.000 sec, 10 frames, 30.000 fps\"\n";
        script << "  i=$((i + 1))\n";
        script << "done\n";
    }
    chmod(fake_sample.c_str(), 0755);

    fs::path input_list = root / "inputs.txt";
    {
        std::ofstream inputs(input_list);
        inputs << "h265:" << (root / "explicit_h265.bin").string() << "\n";
        inputs << "hevc:" << (root / "explicit_hevc.bin").string() << "\n";
        inputs << "h264:" << (root / "explicit_h264.bin").string() << "\n";
        inputs << (root / "auto_ext.h265").string() << "\n";
        inputs << (root / "legacy_default.bin").string() << "\n";
    }

    MFX50_Config cfg{};
    if (require_true(MFX50_DefaultConfig(&cfg) == 0, "transcoder default config")) return 1;
    cfg.route_count = 5;
    cfg.frames_per_route = 10;
    cfg.device_count = 1;
    cfg.devices[0].device_path = "/dev/dri/renderD129";
    cfg.devices[0].route_count = 5;
    cfg.write_outputs = 0;
    std::string fake_sample_str = fake_sample.string();
    cfg.sample_path = fake_sample_str.c_str();

    MFX50_Handle h = MFX50_Create(&cfg);
    if (require_true(h != nullptr, "transcoder create")) return 1;
    fs::path out_dir = root / "out";
    int rc = MFX50_RunInputList(h, input_list.string().c_str(), out_dir.string().c_str());
    if (require_true(rc == 0, "transcoder run input list")) return 1;

    MFX50_Stats stats{};
    if (require_true(MFX50_GetStats(h, &stats) == 0, "transcoder get stats")) return 1;
    std::ifstream par(stats.par_path);
    if (require_true(par.good(), "par file readable")) return 1;
    std::string text((std::istreambuf_iterator<char>(par)), std::istreambuf_iterator<char>());

    if (require_true(contains(text, "-i::h265 " + (root / "explicit_h265.bin").string()), "explicit h265")) return 1;
    if (require_true(contains(text, "-i::h265 " + (root / "explicit_hevc.bin").string()), "explicit hevc")) return 1;
    if (require_true(contains(text, "-i::h264 " + (root / "explicit_h264.bin").string()), "explicit h264")) return 1;
    if (require_true(contains(text, "-i::h265 " + (root / "auto_ext.h265").string()), "auto extension h265")) return 1;
    if (require_true(contains(text, "-i::h264 " + (root / "legacy_default.bin").string()), "legacy default h264")) return 1;

    MFX50_Close(h);
    fs::remove_all(root);
    return 0;
#endif
}

#include "mfx50rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct RouteBench {
    std::string path;
    std::atomic<uint64_t> input_packets{0};
    std::atomic<uint64_t> input_bytes{0};
    std::atomic<uint64_t> output_packets{0};
    std::atomic<uint64_t> output_bytes{0};
    std::atomic<int> errors{0};
    std::atomic<int> demux_failed{0};
};

struct InputExperimentOptions {
    std::string raw;
    std::string filter_chain;
    std::string h264_preset = "veryfast";
    int h264_crf = 18;
    int width = 1920;
    int height = 1080;
    int fps_num = 25;
    int fps_den = 1;
};

bool push_packet_with_retry(MFX50RT_Handle h,
                            uint32_t stream_id,
                            const uint8_t* data,
                            uint32_t size,
                            int64_t pts,
                            uint32_t flags,
                            RouteBench* route) {
    constexpr auto kMaxBackpressureWait = std::chrono::seconds(30);
    const auto start = Clock::now();
    MFX50RT_InputPacket pkt{};
    pkt.size = sizeof(pkt);
    pkt.version = MFX50RT_API_VERSION;
    pkt.stream_id = stream_id;
    pkt.data = data;
    pkt.data_size = size;
    pkt.pts = pts;
    pkt.dts = pts;
    pkt.flags = flags;

    for (;;) {
        MFX50RT_Status st = MFX50RT_PushPacket(h, &pkt);
        if (st == MFX50RT_OK) return true;
        if (st != MFX50RT_ERR_AGAIN) {
            route->errors++;
            return false;
        }
        if (Clock::now() - start > kMaxBackpressureWait) {
            route->errors++;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::string csv_quote(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out += ch;
    }
    out += '"';
    return out;
}

bool has_annexb_suffix(const std::string& path) {
    const std::string lower = [&]() {
        std::string s = path;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }();
    return lower.size() >= 5 &&
           (lower.rfind(".h264") == lower.size() - 5 ||
            lower.rfind(".264") == lower.size() - 4);
}

const char* input_reference_type_for(const std::string& path,
                                     bool onevpl_backend,
                                     const InputExperimentOptions& input_options) {
    if (!input_options.filter_chain.empty()) return "ffmpeg_filtered_h264_annexb";
    if (onevpl_backend && !has_annexb_suffix(path)) return "demuxed_h264_annexb";
    if (has_annexb_suffix(path)) return "h264_annexb_file";
    return "input_file_bytes";
}

std::string trim(std::string value) {
    const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
    const auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

std::vector<std::pair<std::string, std::string>> parse_key_value_list(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = text.find_first_of(",;", pos);
        std::string part = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? text.size() : next + 1;
        const size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(part.substr(0, eq));
        std::string value = trim(part.substr(eq + 1));
        if (!key.empty() && !value.empty()) out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

int numeric_bool_value(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "true" || lower == "yes" || lower == "on") return 1;
    if (lower == "false" || lower == "no" || lower == "off") return 0;
    return std::atoi(value.c_str());
}

std::string join_filter_chain(const std::vector<std::string>& filters) {
    std::ostringstream out;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i > 0) out << ',';
        out << filters[i];
    }
    return out.str();
}

bool parse_resolution(const std::string& value, int* width, int* height) {
    const size_t sep = value.find_first_of("xX:");
    if (sep == std::string::npos) return false;
    const int w = std::atoi(value.substr(0, sep).c_str());
    const int h = std::atoi(value.substr(sep + 1).c_str());
    if (w <= 0 || h <= 0) return false;
    *width = w;
    *height = h;
    return true;
}

bool parse_fps(const std::string& value, int* num, int* den) {
    const size_t sep = value.find('/');
    if (sep != std::string::npos) {
        const int n = std::atoi(value.substr(0, sep).c_str());
        const int d = std::atoi(value.substr(sep + 1).c_str());
        if (n <= 0 || d <= 0) return false;
        const int g = std::gcd(n, d);
        *num = n / g;
        *den = d / g;
        return true;
    }
    const double fps = std::atof(value.c_str());
    if (fps <= 0.0) return false;
    constexpr int kDen = 1000;
    int n = static_cast<int>(fps * kDen + 0.5);
    if (n <= 0) return false;
    const int g = std::gcd(n, kDen);
    *num = n / g;
    *den = kDen / g;
    return true;
}

std::string fps_filter_value(int num, int den) {
    if (den <= 1) return std::to_string(num);
    return std::to_string(num) + "/" + std::to_string(den);
}

std::string denoise_filter_for(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.empty() || lower == "0" || lower == "false" || lower == "off" || lower == "none") {
        return {};
    }
    if (lower == "light") return "hqdn3d=1.2:1.2:4.0:4.0";
    if (lower == "medium") return "hqdn3d=2.0:2.0:6.0:6.0";
    if (lower == "strong") return "hqdn3d=3.0:3.0:9.0:9.0";
    const int strength = std::max(1, std::min(100, std::atoi(value.c_str())));
    const double spatial = std::max(0.4, strength * 0.04);
    const double temporal = std::max(1.0, strength * 0.16);
    std::ostringstream filter;
    filter << "hqdn3d=" << spatial << ':' << spatial << ':' << temporal << ':' << temporal;
    return filter.str();
}

std::string smooth_filter_for(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.empty() || lower == "0" || lower == "false" || lower == "off" || lower == "none") {
        return {};
    }
    int radius = 1;
    if (lower == "medium") radius = 2;
    else if (lower == "strong") radius = 3;
    else if (lower != "light") radius = std::max(1, std::min(4, std::atoi(value.c_str()) / 25));
    std::ostringstream filter;
    filter << "boxblur=" << radius << ":1";
    return filter.str();
}

std::string static_reuse_filter_for(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.empty() || lower == "0" || lower == "false" || lower == "off" || lower == "none") {
        return {};
    }
    if (lower == "aggressive") return "mpdecimate=max=8:hi=768:lo=320:frac=0.20";
    if (lower == "balanced" || lower == "medium") return "mpdecimate=max=5:hi=512:lo=256:frac=0.10";
    if (lower == "loose") return "mpdecimate=max=5:hi=640:lo=288:frac=0.15";
    return "mpdecimate=max=3:hi=384:lo=192:frac=0.05";
}

bool safe_preset_name(const std::string& value) {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') return false;
    }
    return true;
}

InputExperimentOptions parse_input_options(const std::string& text) {
    InputExperimentOptions options;
    options.raw = text;
    std::vector<std::string> filters;
    for (const auto& kv : parse_key_value_list(text)) {
        const std::string& key = kv.first;
        const std::string& value = kv.second;
        if (key == "scale" || key == "resolution") {
            int width = 0;
            int height = 0;
            if (parse_resolution(value, &width, &height)) {
                options.width = width;
                options.height = height;
                filters.push_back("scale=" + std::to_string(width) + ":" + std::to_string(height));
            }
        } else if (key == "width") {
            const int width = std::atoi(value.c_str());
            if (width > 0) options.width = width;
        } else if (key == "height") {
            const int height = std::atoi(value.c_str());
            if (height > 0) options.height = height;
        } else if (key == "fps") {
            int num = 0;
            int den = 0;
            if (parse_fps(value, &num, &den)) {
                options.fps_num = num;
                options.fps_den = den;
                filters.push_back("fps=" + value);
            }
        } else if (key == "reuse_fps" || key == "frame_reuse_fps") {
            int num = 0;
            int den = 0;
            if (parse_fps(value, &num, &den)) {
                filters.push_back("fps=" + fps_filter_value(num, den));
                filters.push_back("fps=" + fps_filter_value(options.fps_num, options.fps_den));
            }
        } else if (key == "static_reuse" || key == "content_reuse") {
            const std::string filter = static_reuse_filter_for(value);
            if (!filter.empty()) {
                filters.push_back(filter);
                filters.push_back("fps=" + fps_filter_value(options.fps_num, options.fps_den));
            }
        } else if (key == "fps_num") {
            const int fps_num = std::atoi(value.c_str());
            if (fps_num > 0) options.fps_num = fps_num;
        } else if (key == "fps_den") {
            const int fps_den = std::atoi(value.c_str());
            if (fps_den > 0) options.fps_den = fps_den;
        } else if (key == "pre_denoise" || key == "denoise") {
            const std::string filter = denoise_filter_for(value);
            if (!filter.empty()) filters.push_back(filter);
        } else if (key == "smooth_scale" || key == "smooth") {
            const std::string filter = smooth_filter_for(value);
            if (!filter.empty()) filters.push_back(filter);
        } else if (key == "ffmpeg_filter" || key == "filter") {
            if (!value.empty() && value != "none") filters.push_back(value);
        } else if (key == "h264_crf") {
            options.h264_crf = std::max(0, std::min(51, std::atoi(value.c_str())));
        } else if (key == "h264_preset") {
            if (safe_preset_name(value)) options.h264_preset = value;
        }
    }
    options.filter_chain = join_filter_chain(filters);
    return options;
}

std::string expert_options_fragment(const std::string& options, MFX50RT_Config* cfg) {
    std::ostringstream out;
    for (const auto& kv : parse_key_value_list(options)) {
        const std::string& key = kv.first;
        const std::string& value = kv.second;
        const int numeric = numeric_bool_value(value);
        if (key == "enable_quality_guard") {
            cfg->algo.enable_quality_guard = numeric;
        } else if (key == "enable_roi") {
            cfg->algo.enable_roi = numeric;
        } else if (key == "enable_spatial_qp") {
            cfg->algo.enable_spatial_qp = numeric;
        } else if (key == "enable_mbqp") {
            cfg->algo.enable_mbqp = numeric;
        } else if (key == "async_mode") {
            cfg->runtime.async_mode = numeric;
        } else if (key == "async_depth") {
            const int depth = std::atoi(value.c_str());
            if (depth > 0) cfg->backend.async_depth = depth;
        } else if (key == "target_usage") {
            // Forwarded through expert_options_json for the oneVPL legacy adapter.
        }
        out << ",\"" << key << "\":" << numeric;
    }
    return out.str();
}

bool ffprobe_hevc_ok(const std::string& path) {
    std::ostringstream cmd;
    cmd << "ffprobe -v error -select_streams v:0 "
        << "-show_entries stream=codec_name -of csv=p=0 "
        << shell_quote(path)
        << " 2>/dev/null";
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return false;
    char buf[128] = {};
    std::string output;
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);
    return output.find("hevc") != std::string::npos;
}

std::vector<std::string> load_list(const char* path, int limit) {
    std::ifstream in(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line) && static_cast<int>(out.size()) < limit) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

void push_route(MFX50RT_Handle h,
                uint32_t stream_id,
                RouteBench* route,
                uint64_t max_bytes,
                size_t chunk_bytes,
                bool onevpl_backend,
                const InputExperimentOptions* input_options) {
    std::vector<uint8_t> chunk(chunk_bytes);
    uint64_t read_total = 0;
    int64_t pts = 0;

    auto consume = [&](const uint8_t* data, size_t got) -> bool {
        if (got == 0) return true;
        if (!push_packet_with_retry(h,
                                    stream_id,
                                    data,
                                    static_cast<uint32_t>(got),
                                    pts,
                                    0,
                                    route)) {
            return false;
        }
        route->input_packets++;
        route->input_bytes += static_cast<uint64_t>(got);
        read_total += static_cast<uint64_t>(got);
        pts++;
        return true;
    };

    if (input_options && !input_options->filter_chain.empty()) {
        std::ostringstream cmd;
        cmd << "ffmpeg -hide_banner -loglevel error -nostdin -i "
            << shell_quote(route->path)
            << " -map 0:v:0 -vf "
            << shell_quote(input_options->filter_chain)
            << " -an -c:v libx264 -preset "
            << shell_quote(input_options->h264_preset)
            << " -crf " << input_options->h264_crf
            << " -pix_fmt yuv420p -f h264 - 2>/dev/null";
        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            route->demux_failed++;
            return;
        }
        while (!std::feof(pipe) && (max_bytes == 0 || read_total < max_bytes)) {
            const uint64_t left = max_bytes == 0 ? chunk.size() : std::min<uint64_t>(chunk.size(), max_bytes - read_total);
            if (left == 0) break;
            const size_t got = std::fread(chunk.data(), 1, static_cast<size_t>(left), pipe);
            if (got == 0) break;
            if (!consume(chunk.data(), got)) break;
        }
        const int close_status = pclose(pipe);
        if (read_total == 0 && close_status != 0) route->demux_failed++;
    } else if (onevpl_backend && !has_annexb_suffix(route->path)) {
        std::ostringstream cmd;
        cmd << "ffmpeg -hide_banner -loglevel error -nostdin -i "
            << shell_quote(route->path)
            << " -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb -f h264 - 2>/dev/null";
        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            route->demux_failed++;
            return;
        }
        while (!std::feof(pipe) && (max_bytes == 0 || read_total < max_bytes)) {
            const uint64_t left = max_bytes == 0 ? chunk.size() : std::min<uint64_t>(chunk.size(), max_bytes - read_total);
            if (left == 0) break;
            const size_t got = std::fread(chunk.data(), 1, static_cast<size_t>(left), pipe);
            if (got == 0) break;
            if (!consume(chunk.data(), got)) break;
        }
        const int close_status = pclose(pipe);
        if (read_total == 0 && close_status != 0) route->demux_failed++;
    } else {
        std::ifstream in(route->path, std::ios::binary);
        if (!in) {
            route->errors++;
            return;
        }
        while (in && (max_bytes == 0 || read_total < max_bytes)) {
            const uint64_t left = max_bytes == 0 ? chunk.size() : std::min<uint64_t>(chunk.size(), max_bytes - read_total);
            if (left == 0) break;
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(left));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            if (!consume(chunk.data(), static_cast<size_t>(got))) break;
        }
    }

    push_packet_with_retry(h,
                           stream_id,
                           nullptr,
                           0,
                           pts,
                           MFX50RT_PACKET_FLAG_EOS,
                           route);
}

void write_output_packet(std::vector<std::unique_ptr<std::ofstream>>& outs,
                         const MFX50RT_OutputPacket& out) {
    if (out.stream_id >= outs.size() || !outs[out.stream_id] || !out.data || out.data_size == 0) return;
    outs[out.stream_id]->write(reinterpret_cast<const char*>(out.data),
                               static_cast<std::streamsize>(out.data_size));
}

void open_route_outputs(const std::string& prefix,
                        int route_count,
                        std::vector<std::unique_ptr<std::ofstream>>* outs) {
    outs->clear();
    outs->reserve(static_cast<size_t>(route_count));
    for (int i = 0; i < route_count; ++i) {
        auto out = std::make_unique<std::ofstream>(
            prefix + std::to_string(i) + ".hevc",
            std::ios::binary | std::ios::trunc);
        outs->push_back(std::move(out));
    }
}

const char* strategy_name(int strategy) {
    switch (strategy) {
        case MFX50RT_STRATEGY_MBQP_CQP: return "MBQP_CQP";
        case MFX50RT_STRATEGY_ROI_DELTA_QP: return "ROI_DELTA_QP";
        case MFX50RT_STRATEGY_GLOBAL: return "GLOBAL";
        default: return "AUTO";
    }
}

const char* route_status_name(const RouteBench& route,
                              bool onevpl_backend,
                              bool ffprobe_ok) {
    if (route.input_bytes.load() == 0) return "INPUT_EMPTY";
    if (route.demux_failed.load() > 0) return "FFMPEG_DEMUX_FAIL";
    if (route.errors.load() > 0) return "SDK_ERROR";
    if (route.output_bytes.load() == 0) return "ENCODE_EMPTY";
    if (onevpl_backend && !ffprobe_ok) return "FFPROBE_FAIL";
    return "OK";
}

bool route_status_ok(const char* status) {
    return std::string(status) == "OK";
}

bool route_status_invalid(const char* status) {
    return std::string(status) == "INPUT_EMPTY";
}

struct TraceSummary {
    uint32_t count = 0;
    double base_scene_qp = 0.0;
    double frame_anchor_qp = 0.0;
    double spatial_avg_qp = 0.0;
    double qp_p50 = 0.0;
    double qp_p90 = 0.0;
    double edge_density = 0.0;
    double motion_score = 0.0;
    double noise_score = 0.0;
    double scene_cut_score = 0.0;
    double hard_scene_like = 0.0;
    double hard_guard_active = 0.0;
    double class_qp_table_overwrite_count = 0.0;
    double static_reuse_candidate = 0.0;
    double static_reuse_consecutive_frames = 0.0;
    double static_reuse_risk_score = 0.0;
    double foreground_ratio = 0.0;
    double roi_block_ratio = 0.0;
    double transition_block_ratio = 0.0;
    double background_block_ratio = 0.0;
    double flat_background_block_ratio = 0.0;
    double foreground_block_ratio = 0.0;
    double edge_block_ratio = 0.0;
    double texture_block_ratio = 0.0;
    double true_roi_block_ratio = 0.0;
    double edge_texture_roi_block_ratio = 0.0;
    double high_texture_background_block_ratio = 0.0;
    double hard_scene_background_block_ratio = 0.0;
    double normal_background_block_ratio = 0.0;
    double high_qp_block_ratio = 0.0;
    double smoothing_changed_qp_avg = 0.0;
    double quality_guard_active = 0.0;

    void add(const MFX50RT_DecisionTrace& t) {
        count++;
        base_scene_qp += t.base_scene_qp;
        frame_anchor_qp += t.frame_anchor_qp;
        spatial_avg_qp += t.spatial_avg_qp;
        qp_p50 += t.qp_p50;
        qp_p90 += t.qp_p90;
        edge_density += t.edge_density;
        motion_score += t.motion_score;
        noise_score += t.noise_score;
        scene_cut_score += t.scene_cut_score;
        foreground_ratio += t.foreground_ratio;
        roi_block_ratio += t.roi_block_ratio;
        transition_block_ratio += t.transition_block_ratio;
        background_block_ratio += t.background_block_ratio;
        flat_background_block_ratio += t.flat_background_block_ratio;
        foreground_block_ratio += t.foreground_block_ratio;
        edge_block_ratio += t.edge_block_ratio;
        texture_block_ratio += t.texture_block_ratio;
        true_roi_block_ratio += t.true_roi_block_ratio;
        edge_texture_roi_block_ratio += t.edge_texture_roi_block_ratio;
        high_texture_background_block_ratio += t.high_texture_background_block_ratio;
        hard_scene_background_block_ratio += t.hard_scene_background_block_ratio;
        normal_background_block_ratio += t.normal_background_block_ratio;
        high_qp_block_ratio += t.high_qp_block_ratio;
        smoothing_changed_qp_avg += t.smoothing_changed_qp_avg;
        quality_guard_active += t.quality_guard_state ? 1.0 : 0.0;

        hard_scene_like += t.hard_scene_like ? 1.0 : 0.0;
        hard_guard_active += t.hard_guard_active ? 1.0 : 0.0;
        class_qp_table_overwrite_count += t.class_qp_table_overwrite_count;
        static_reuse_candidate += t.static_reuse_candidate ? 1.0 : 0.0;
        static_reuse_consecutive_frames += t.static_reuse_consecutive_frames;
        static_reuse_risk_score += t.static_reuse_risk_score;
    }

    double avg(double value) const {
        return count > 0 ? value / static_cast<double>(count) : 0.0;
    }
};

void write_trace_summary(MFX50RT_Handle h,
                         const std::vector<RouteBench>& routes,
                         const char* path) {
    std::ofstream csv(path, std::ios::trunc);
    csv << "route,trace_count,base_scene_qp_avg,frame_anchor_qp_avg,"
        << "spatial_avg_qp_avg,qp_p50_avg,qp_p90_avg,edge_density_avg,"
        << "motion_score_avg,noise_score_avg,scene_cut_score_avg,"
        << "hard_scene_like_ratio,hard_guard_active_ratio,"
        << "class_qp_table_overwrite_count_avg,"
        << "static_reuse_candidate_ratio,static_reuse_consecutive_frames_avg,"
        << "static_reuse_risk_score_avg,"
        << "foreground_ratio_avg,roi_block_ratio_avg,"
        << "transition_block_ratio_avg,background_block_ratio_avg,"
        << "flat_background_block_ratio_avg,"
        << "foreground_block_ratio_avg,edge_block_ratio_avg,texture_block_ratio_avg,"
        << "true_roi_block_ratio_avg,edge_texture_roi_block_ratio_avg,"
        << "high_texture_background_block_ratio_avg,"
        << "hard_scene_background_block_ratio_avg,normal_background_block_ratio_avg,"
        << "high_qp_block_ratio_avg,smoothing_changed_qp_avg,"
        << "quality_guard_active_ratio,path\n";
    for (uint32_t stream_id = 0; stream_id < routes.size(); ++stream_id) {
        uint32_t count = 0;
        MFX50RT_Status st = MFX50RT_GetDecisionTrace(h, stream_id, nullptr, &count);
        std::vector<MFX50RT_DecisionTrace> traces(count);
        if (st == MFX50RT_OK && count > 0) {
            st = MFX50RT_GetDecisionTrace(h, stream_id, traces.data(), &count);
        }
        TraceSummary summary;
        if (st == MFX50RT_OK) {
            for (uint32_t i = 0; i < count; ++i) summary.add(traces[i]);
        }
        csv << stream_id << ','
            << summary.count << ','
            << summary.avg(summary.base_scene_qp) << ','
            << summary.avg(summary.frame_anchor_qp) << ','
            << summary.avg(summary.spatial_avg_qp) << ','
            << summary.avg(summary.qp_p50) << ','
            << summary.avg(summary.qp_p90) << ','
            << summary.avg(summary.edge_density) << ','
            << summary.avg(summary.motion_score) << ','
            << summary.avg(summary.noise_score) << ','
            << summary.avg(summary.scene_cut_score) << ','
            << summary.avg(summary.hard_scene_like) << ','
            << summary.avg(summary.hard_guard_active) << ','
            << summary.avg(summary.class_qp_table_overwrite_count) << ','
            << summary.avg(summary.static_reuse_candidate) << ','
            << summary.avg(summary.static_reuse_consecutive_frames) << ','
            << summary.avg(summary.static_reuse_risk_score) << ','
            << summary.avg(summary.foreground_ratio) << ','
            << summary.avg(summary.roi_block_ratio) << ','
            << summary.avg(summary.transition_block_ratio) << ','
            << summary.avg(summary.background_block_ratio) << ','
            << summary.avg(summary.flat_background_block_ratio) << ','
            << summary.avg(summary.foreground_block_ratio) << ','
            << summary.avg(summary.edge_block_ratio) << ','
            << summary.avg(summary.texture_block_ratio) << ','
            << summary.avg(summary.true_roi_block_ratio) << ','
            << summary.avg(summary.edge_texture_roi_block_ratio) << ','
            << summary.avg(summary.high_texture_background_block_ratio) << ','
            << summary.avg(summary.hard_scene_background_block_ratio) << ','
            << summary.avg(summary.normal_background_block_ratio) << ','
            << summary.avg(summary.high_qp_block_ratio) << ','
            << summary.avg(summary.smoothing_changed_qp_avg) << ','
            << summary.avg(summary.quality_guard_active) << ",\""
            << routes[stream_id].path << "\"\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <video_list.txt> [max_bytes_per_route] [chunk_bytes] [route_count] [null|onevpl] [output_prefix] [force_mbqp_pattern] [profile] [gop_size] [idr_interval] [b_frames] [expert_options] [input_options]\n",
                     argv[0]);
        return 2;
    }
    const uint64_t max_bytes = argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 64ull * 1024ull * 1024ull;
    const size_t chunk_bytes = argc >= 4 ? static_cast<size_t>(std::strtoull(argv[3], nullptr, 10)) : 256ull * 1024ull;
    const int route_count = argc >= 5 ? std::atoi(argv[4]) : 45;
    const std::string backend = argc >= 6 ? argv[5] : "null";
    const std::string output_prefix = argc >= 7 ? argv[6] : "build_hybridtsrq/real_45_route_";
    const std::string force_mbqp_pattern = argc >= 8 ? argv[7] : "none";
    const std::string profile = argc >= 9 ? argv[8] : "target_90_ssim_guard";
    const int gop_size = argc >= 10 ? std::atoi(argv[9]) : 60;
    const int idr_interval = argc >= 11 ? std::atoi(argv[10]) : 120;
    const int b_frames = argc >= 12 ? std::atoi(argv[11]) : 0;
    const std::string expert_options = argc >= 13 ? argv[12] : "";
    const InputExperimentOptions input_options = parse_input_options(argc >= 14 ? argv[13] : "");
    const bool onevpl_backend = backend == "onevpl";
    if (route_count <= 0 || chunk_bytes == 0) return 2;

    std::vector<std::string> paths = load_list(argv[1], route_count);
    if (static_cast<int>(paths.size()) != route_count) {
        std::fprintf(stderr, "need %d videos, got %zu\n", route_count, paths.size());
        return 2;
    }

    MFX50RT_Config cfg{};
    if (MFX50RT_DefaultConfig(&cfg) != MFX50RT_OK) return 1;
    cfg.backend.type = onevpl_backend ? MFX50RT_BACKEND_ONEVPL : MFX50RT_BACKEND_NULL;
    cfg.runtime.route_count = route_count;
    cfg.runtime.worker_threads = route_count;
    cfg.runtime.async_mode = 1;
    cfg.runtime.queue_depth_per_route = 64;
    cfg.algo.strategy = onevpl_backend ? MFX50RT_STRATEGY_AUTO : MFX50RT_STRATEGY_GLOBAL;
    cfg.algo.target_compression_percent = 90;
    cfg.pipeline.input_codec = MFX50RT_CODEC_H264;
    cfg.pipeline.output_codec = MFX50RT_CODEC_HEVC;
    cfg.pipeline.width = input_options.width;
    cfg.pipeline.height = input_options.height;
    cfg.pipeline.fps_num = input_options.fps_num;
    cfg.pipeline.fps_den = input_options.fps_den;
    cfg.pipeline.gop_size = gop_size;
    cfg.pipeline.idr_interval = idr_interval;
    cfg.pipeline.b_frames = b_frames;
    cfg.pipeline.low_latency = b_frames > 0 ? 0 : 1;
    cfg.backend.low_latency = b_frames > 0 ? 0 : 1;
    const std::string expert_fragment = expert_options_fragment(expert_options, &cfg);
    std::snprintf(cfg.algo.expert_options_json,
                  sizeof(cfg.algo.expert_options_json),
                  "{\"algorithm\":{\"profile\":\"%s\"},"
                  "\"debug\":{\"force_mbqp_pattern\":\"%s\"}%s}",
                  profile.c_str(),
                  force_mbqp_pattern.c_str(),
                  expert_fragment.c_str());

    MFX50RT_Handle h = nullptr;
    MFX50RT_Status st = MFX50RT_Create(&cfg, &h);
    if (st != MFX50RT_OK) {
        std::fprintf(stderr, "create failed: %s\n", MFX50RT_StatusString(st));
        return 1;
    }

    MFX50RT_EffectiveConfig eff{};
    eff.size = sizeof(eff);
    eff.version = MFX50RT_API_VERSION;
    MFX50RT_GetEffectiveConfig(h, &eff);

    std::vector<RouteBench> routes(static_cast<size_t>(route_count));
    for (int i = 0; i < route_count; ++i) routes[static_cast<size_t>(i)].path = paths[static_cast<size_t>(i)];
    std::vector<std::unique_ptr<std::ofstream>> route_outputs;
    if (onevpl_backend) open_route_outputs(output_prefix, route_count, &route_outputs);

    const auto start = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(route_count));
    for (int i = 0; i < route_count; ++i) {
        threads.emplace_back(push_route,
                             h,
                             static_cast<uint32_t>(i),
                             &routes[static_cast<size_t>(i)],
                             max_bytes,
                             chunk_bytes,
                             onevpl_backend,
                             &input_options);
    }

    std::atomic<bool> pushers_done{false};
    std::thread joiner([&]() {
        for (auto& t : threads) t.join();
        pushers_done.store(true);
    });

    uint64_t outputs = 0;
    int idle_polls = 0;
    uint64_t last_encoded = 0;
    bool flushed = false;
    while (true) {
        uint64_t pushed = 0;
        for (const auto& r : routes) pushed += r.input_packets.load();
        uint64_t encoded = 0;
        for (int i = 0; i < route_count; ++i) {
            MFX50RT_RouteStats stats{};
            stats.size = sizeof(stats);
            stats.version = MFX50RT_API_VERSION;
            if (MFX50RT_GetRouteStats(h, static_cast<uint32_t>(i), &stats) == MFX50RT_OK) {
                encoded += stats.output_packets;
            }
        }
        if (encoded != last_encoded) {
            idle_polls = 0;
            last_encoded = encoded;
        }

        MFX50RT_OutputPacket out{};
        st = MFX50RT_PollPacket(h, &out, 100);
        if (st == MFX50RT_OK) {
            if (out.stream_id < static_cast<uint32_t>(routes.size())) {
                auto& r = routes[out.stream_id];
                r.output_packets++;
                r.output_bytes += out.data_size;
            }
            if (onevpl_backend) write_output_packet(route_outputs, out);
            outputs++;
            idle_polls = 0;
            MFX50RT_ReleasePacket(h, &out);
        } else if (st == MFX50RT_ERR_AGAIN) {
            idle_polls++;
        } else {
            std::fprintf(stderr, "poll failed: %s\n", MFX50RT_StatusString(st));
            break;
        }

        if (pushers_done.load() && !flushed) {
            MFX50RT_Flush(h, MFX50RT_STREAM_ALL);
            flushed = true;
            idle_polls = 0;
        }
        if (!onevpl_backend && pushers_done.load() && encoded >= pushed && outputs >= pushed) break;
        const int idle_limit = onevpl_backend ? 30 : 600;
        if (pushers_done.load() && flushed && idle_polls > idle_limit) {
            break;
        }
    }
    joiner.join();
    if (!flushed) MFX50RT_Flush(h, MFX50RT_STREAM_ALL);
    for (auto& out : route_outputs) {
        if (out) out->close();
    }

    std::vector<MFX50RT_RouteStats> sdk_stats(static_cast<size_t>(route_count));
    uint64_t total_encoded_frames = 0;
    uint64_t total_mbqp_applied = 0;
    uint64_t total_mbqp_fallback = 0;
    uint64_t total_mbqp_skipped = 0;
    int qp_min = 0;
    int qp_max = 0;
    uint64_t qp_avg_sum = 0;
    uint64_t qp_avg_count = 0;
    uint64_t qp_p50_sum = 0;
    uint64_t qp_p90_sum = 0;
    uint64_t qp_p95_sum = 0;
    uint64_t qp_percentile_count = 0;
    double high_qp_ratio_sum = 0.0;
    double background_ratio_sum = 0.0;
    double flat_background_ratio_sum = 0.0;
    double roi_ratio_sum = 0.0;
    uint64_t block_ratio_count = 0;
    double fps_out_sum = 0.0;
    double latency_p95_sum = 0.0;
    double latency_p99_sum = 0.0;
    uint64_t latency_count = 0;
    uint32_t max_input_queue_depth = 0;
    uint32_t max_output_queue_depth = 0;
    std::string actual_encode_control = "GLOBAL";
    for (int i = 0; i < route_count; ++i) {
        auto& stats = sdk_stats[static_cast<size_t>(i)];
        stats.size = sizeof(stats);
        stats.version = MFX50RT_API_VERSION;
        if (MFX50RT_GetRouteStats(h, static_cast<uint32_t>(i), &stats) != MFX50RT_OK) continue;
        total_encoded_frames += stats.encoded_frames;
        total_mbqp_applied += stats.mbqp_applied_frames;
        total_mbqp_fallback += stats.mbqp_fallback_count;
        total_mbqp_skipped += stats.mbqp_skipped_frames;
        fps_out_sum += stats.fps_out;
        if (stats.latency_ms_p95 > 0.0 || stats.latency_ms_p99 > 0.0) {
            latency_p95_sum += stats.latency_ms_p95;
            latency_p99_sum += stats.latency_ms_p99;
            latency_count++;
        }
        max_input_queue_depth = std::max(max_input_queue_depth, stats.queue_depth_input);
        max_output_queue_depth = std::max(max_output_queue_depth, stats.queue_depth_output);
        if (stats.actual_encode_control[0] != '\0' &&
            std::string(stats.actual_encode_control) != "GLOBAL") {
            actual_encode_control = stats.actual_encode_control;
        }
        if (stats.qp_min > 0 && (qp_min == 0 || stats.qp_min < qp_min)) qp_min = stats.qp_min;
        if (stats.qp_max > qp_max) qp_max = stats.qp_max;
        if (stats.qp_avg > 0) {
            qp_avg_sum += static_cast<uint64_t>(stats.qp_avg);
            qp_avg_count++;
        }
        if (stats.qp_p50 > 0) {
            qp_p50_sum += static_cast<uint64_t>(stats.qp_p50);
            qp_p90_sum += static_cast<uint64_t>(stats.qp_p90);
            qp_p95_sum += static_cast<uint64_t>(stats.qp_p95);
            qp_percentile_count++;
        }
        if (stats.background_block_ratio > 0.0 ||
            stats.flat_background_block_ratio > 0.0 ||
            stats.roi_block_ratio > 0.0 ||
            stats.high_qp_block_ratio > 0.0) {
            high_qp_ratio_sum += stats.high_qp_block_ratio;
            background_ratio_sum += stats.background_block_ratio;
            flat_background_ratio_sum += stats.flat_background_block_ratio;
            roi_ratio_sum += stats.roi_block_ratio;
            block_ratio_count++;
        }
    }
    const int qp_avg = qp_avg_count > 0 ? static_cast<int>(qp_avg_sum / qp_avg_count) : 0;
    const int qp_p50 = qp_percentile_count > 0 ? static_cast<int>(qp_p50_sum / qp_percentile_count) : 0;
    const int qp_p90 = qp_percentile_count > 0 ? static_cast<int>(qp_p90_sum / qp_percentile_count) : 0;
    const int qp_p95 = qp_percentile_count > 0 ? static_cast<int>(qp_p95_sum / qp_percentile_count) : 0;
    const double high_qp_block_ratio =
        block_ratio_count > 0 ? high_qp_ratio_sum / block_ratio_count : 0.0;
    const double background_block_ratio =
        block_ratio_count > 0 ? background_ratio_sum / block_ratio_count : 0.0;
    const double flat_background_block_ratio =
        block_ratio_count > 0 ? flat_background_ratio_sum / block_ratio_count : 0.0;
    const double roi_block_ratio =
        block_ratio_count > 0 ? roi_ratio_sum / block_ratio_count : 0.0;
    const double fps_out_avg = route_count > 0 ? fps_out_sum / route_count : 0.0;
    const double latency_ms_p95_avg =
        latency_count > 0 ? latency_p95_sum / static_cast<double>(latency_count) : 0.0;
    const double latency_ms_p99_avg =
        latency_count > 0 ? latency_p99_sum / static_cast<double>(latency_count) : 0.0;

    int ffprobe_ok = 0;
    int ffprobe_bad = 0;
    std::vector<int> route_ffprobe_ok(static_cast<size_t>(route_count), onevpl_backend ? 0 : 1);
    if (onevpl_backend) {
        for (int i = 0; i < route_count; ++i) {
            const auto& r = routes[static_cast<size_t>(i)];
            if (r.input_bytes.load() == 0) {
                route_ffprobe_ok[static_cast<size_t>(i)] = 1;
                continue;
            }
            const std::string out_path = output_prefix + std::to_string(i) + ".hevc";
            if (ffprobe_hevc_ok(out_path)) {
                route_ffprobe_ok[static_cast<size_t>(i)] = 1;
                ffprobe_ok++;
            } else {
                ffprobe_bad++;
            }
        }
    }

    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    uint64_t total_packets = 0;
    uint64_t total_errors = 0;
    for (const auto& r : routes) {
        total_in += r.input_bytes.load();
        total_out += r.output_bytes.load();
        total_packets += r.input_packets.load();
        total_errors += static_cast<uint64_t>(r.errors.load());
    }

    uint64_t valid_routes = 0;
    uint64_t invalid_routes = 0;
    uint64_t failed_routes = 0;
    for (int i = 0; i < route_count; ++i) {
        const auto& r = routes[static_cast<size_t>(i)];
        const char* status = route_status_name(r,
                                               onevpl_backend,
                                               route_ffprobe_ok[static_cast<size_t>(i)] != 0);
        if (route_status_ok(status)) valid_routes++;
        else if (route_status_invalid(status)) invalid_routes++;
        else failed_routes++;
    }

    std::ofstream csv("build_hybridtsrq/real_45_files_report.csv", std::ios::trunc);
    csv << "route,input_reference_type,input_reference_bytes,output_hevc_bytes,"
        << "input_options,input_filter,pipeline_width,pipeline_height,"
        << "pipeline_fps_num,pipeline_fps_den,"
        << "input_bytes,output_bytes,input_packets,output_packets,encoded_frames,"
        << "mbqp_applied_frames,mbqp_fallback_count,actual_encode_control,"
        << "fps_out,latency_ms_p95,latency_ms_p99,queue_depth_input,queue_depth_output,"
        << "qp_min,qp_avg,qp_max,qp_p50,qp_p90,qp_p95,high_qp_block_ratio,"
        << "background_block_ratio,flat_background_block_ratio,roi_block_ratio,"
        << "edge_block_ratio,transition_block_ratio,true_roi_block_ratio,"
        << "edge_texture_roi_block_ratio,high_texture_background_block_ratio,"
        << "hard_scene_background_block_ratio,normal_background_block_ratio,"
        << "smoothing_changed_qp_avg,"
        << "compression_ratio,route_status,errors,path\n";
    for (int i = 0; i < route_count; ++i) {
        const auto& r = routes[static_cast<size_t>(i)];
        const auto& stats = sdk_stats[static_cast<size_t>(i)];
        const char* status = route_status_name(r,
                                               onevpl_backend,
                                               route_ffprobe_ok[static_cast<size_t>(i)] != 0);
        const double ratio = r.input_bytes.load() > 0
            ? 1.0 - static_cast<double>(r.output_bytes.load()) / static_cast<double>(r.input_bytes.load())
            : 0.0;
        csv << i << ','
            << input_reference_type_for(r.path, onevpl_backend, input_options) << ','
            << r.input_bytes.load() << ','
            << r.output_bytes.load() << ','
            << csv_quote(input_options.raw.empty() ? "none" : input_options.raw) << ','
            << csv_quote(input_options.filter_chain.empty() ? "none" : input_options.filter_chain) << ','
            << input_options.width << ','
            << input_options.height << ','
            << input_options.fps_num << ','
            << input_options.fps_den << ','
            << r.input_bytes.load() << ','
            << r.output_bytes.load() << ','
            << r.input_packets.load() << ','
            << r.output_packets.load() << ','
            << stats.encoded_frames << ','
            << stats.mbqp_applied_frames << ','
            << stats.mbqp_fallback_count << ','
            << stats.actual_encode_control << ','
            << stats.fps_out << ','
            << stats.latency_ms_p95 << ','
            << stats.latency_ms_p99 << ','
            << stats.queue_depth_input << ','
            << stats.queue_depth_output << ','
            << stats.qp_min << ','
            << stats.qp_avg << ','
            << stats.qp_max << ','
            << stats.qp_p50 << ','
            << stats.qp_p90 << ','
            << stats.qp_p95 << ','
            << stats.high_qp_block_ratio << ','
            << stats.background_block_ratio << ','
            << stats.flat_background_block_ratio << ','
            << stats.roi_block_ratio << ','
            << stats.edge_block_ratio << ','
            << stats.transition_block_ratio << ','
            << stats.true_roi_block_ratio << ','
            << stats.edge_texture_roi_block_ratio << ','
            << stats.high_texture_background_block_ratio << ','
            << stats.hard_scene_background_block_ratio << ','
            << stats.normal_background_block_ratio << ','
            << stats.smoothing_changed_qp_avg << ','
            << ratio << ','
            << status << ','
            << r.errors.load() << ','
            << csv_quote(r.path) << "\n";
    }
    csv.close();

    std::ofstream md("build_hybridtsrq/real_45_files_report.md", std::ios::trunc);
    md << "# MFX50RT Real File 45 Route Smoke\n\n";
    md << "- route_count: " << route_count << "\n";
    md << "- backend: " << backend << "\n";
    if (onevpl_backend) md << "- output_prefix: " << output_prefix << "\n";
    md << "- force_mbqp_pattern: " << force_mbqp_pattern << "\n";
    md << "- profile: " << profile << "\n";
    md << "- gop_size: " << gop_size << "\n";
    md << "- idr_interval: " << idr_interval << "\n";
    md << "- b_frames: " << b_frames << "\n";
    md << "- expert_options: " << (expert_options.empty() ? "none" : expert_options) << "\n";
    md << "- input_options: " << (input_options.raw.empty() ? "none" : input_options.raw) << "\n";
    md << "- input_filter: " << (input_options.filter_chain.empty() ? "none" : input_options.filter_chain) << "\n";
    md << "- pipeline_width: " << input_options.width << "\n";
    md << "- pipeline_height: " << input_options.height << "\n";
    md << "- pipeline_fps_num: " << input_options.fps_num << "\n";
    md << "- pipeline_fps_den: " << input_options.fps_den << "\n";
    md << "- max_bytes_per_route: " << max_bytes << "\n";
    md << "- chunk_bytes: " << chunk_bytes << "\n";
    md << "- effective_strategy: " << strategy_name(eff.effective_strategy) << "\n";
    md << "- actual_encode_control: " << actual_encode_control << "\n";
    md << "- fallback_reason: " << eff.fallback_reason << "\n";
    md << "- elapsed_seconds: " << elapsed << "\n";
    md << "- input_reference_type: "
       << (paths.empty() ? "unknown" : input_reference_type_for(paths.front(), onevpl_backend, input_options)) << "\n";
    md << "- input_reference_bytes: " << total_in << "\n";
    md << "- output_hevc_bytes: " << total_out << "\n";
    md << "- compression_ratio_formula: 1 - output_hevc_bytes / input_reference_bytes\n";
    md << "- compression_ratio: " << (total_in ? 1.0 - static_cast<double>(total_out) / total_in : 0.0) << "\n";
    md << "- input_bytes: " << total_in << "\n";
    md << "- output_bytes: " << total_out << "\n";
    md << "- input_packets: " << total_packets << "\n";
    md << "- encoded_frames: " << total_encoded_frames << "\n";
    md << "- output_packets: " << outputs << "\n";
    md << "- valid_routes: " << valid_routes << "\n";
    md << "- invalid_routes: " << invalid_routes << "\n";
    md << "- failed_routes: " << failed_routes << "\n";
    md << "- mbqp_applied_frames: " << total_mbqp_applied << "\n";
    md << "- mbqp_fallback_count: " << total_mbqp_fallback << "\n";
    md << "- mbqp_skipped_frames: " << total_mbqp_skipped << "\n";
    md << "- qp_min: " << qp_min << "\n";
    md << "- qp_avg: " << qp_avg << "\n";
    md << "- qp_max: " << qp_max << "\n";
    md << "- qp_p50: " << qp_p50 << "\n";
    md << "- qp_p90: " << qp_p90 << "\n";
    md << "- qp_p95: " << qp_p95 << "\n";
    md << "- fps_out_avg: " << fps_out_avg << "\n";
    md << "- latency_ms_p95_avg: " << latency_ms_p95_avg << "\n";
    md << "- latency_ms_p99_avg: " << latency_ms_p99_avg << "\n";
    md << "- max_input_queue_depth: " << max_input_queue_depth << "\n";
    md << "- max_output_queue_depth: " << max_output_queue_depth << "\n";
    md << "- high_qp_block_ratio: " << high_qp_block_ratio << "\n";
    md << "- background_block_ratio: " << background_block_ratio << "\n";
    md << "- flat_background_block_ratio: " << flat_background_block_ratio << "\n";
    md << "- roi_block_ratio: " << roi_block_ratio << "\n";
    md << "- ffprobe_hevc_ok: " << ffprobe_ok << "\n";
    md << "- ffprobe_bad: " << ffprobe_bad << "\n";
    md << "- errors: " << total_errors << "\n";
    md.close();

    write_trace_summary(h, routes, "build_hybridtsrq/real_45_files_trace.csv");

    MFX50RT_Close(h);

    std::printf("routes=%d\n", route_count);
    std::printf("backend=%s\n", backend.c_str());
    std::printf("force_mbqp_pattern=%s\n", force_mbqp_pattern.c_str());
    std::printf("profile=%s\n", profile.c_str());
    std::printf("gop_size=%d\n", gop_size);
    std::printf("idr_interval=%d\n", idr_interval);
    std::printf("b_frames=%d\n", b_frames);
    std::printf("expert_options=%s\n", expert_options.empty() ? "none" : expert_options.c_str());
    std::printf("input_options=%s\n", input_options.raw.empty() ? "none" : input_options.raw.c_str());
    std::printf("input_filter=%s\n", input_options.filter_chain.empty() ? "none" : input_options.filter_chain.c_str());
    std::printf("pipeline_width=%d\n", input_options.width);
    std::printf("pipeline_height=%d\n", input_options.height);
    std::printf("pipeline_fps_num=%d\n", input_options.fps_num);
    std::printf("pipeline_fps_den=%d\n", input_options.fps_den);
    std::printf("effective_strategy=%s\n", strategy_name(eff.effective_strategy));
    std::printf("actual_encode_control=%s\n", actual_encode_control.c_str());
    std::printf("fallback_reason=%s\n", eff.fallback_reason);
    std::printf("elapsed_seconds=%.3f\n", elapsed);
    std::printf("input_reference_type=%s\n",
                paths.empty() ? "unknown" : input_reference_type_for(paths.front(), onevpl_backend, input_options));
    std::printf("input_reference_bytes=%" PRIu64 "\n", total_in);
    std::printf("output_hevc_bytes=%" PRIu64 "\n", total_out);
    std::printf("compression_ratio_formula=1 - output_hevc_bytes / input_reference_bytes\n");
    std::printf("compression_ratio=%.6f\n", total_in ? 1.0 - static_cast<double>(total_out) / total_in : 0.0);
    std::printf("input_bytes=%" PRIu64 "\n", total_in);
    std::printf("output_bytes=%" PRIu64 "\n", total_out);
    std::printf("input_packets=%" PRIu64 "\n", total_packets);
    std::printf("encoded_frames=%" PRIu64 "\n", total_encoded_frames);
    std::printf("output_packets=%" PRIu64 "\n", outputs);
    std::printf("valid_routes=%" PRIu64 "\n", valid_routes);
    std::printf("invalid_routes=%" PRIu64 "\n", invalid_routes);
    std::printf("failed_routes=%" PRIu64 "\n", failed_routes);
    std::printf("mbqp_applied_frames=%" PRIu64 "\n", total_mbqp_applied);
    std::printf("mbqp_fallback_count=%" PRIu64 "\n", total_mbqp_fallback);
    std::printf("mbqp_skipped_frames=%" PRIu64 "\n", total_mbqp_skipped);
    std::printf("qp_min=%d\n", qp_min);
    std::printf("qp_avg=%d\n", qp_avg);
    std::printf("qp_max=%d\n", qp_max);
    std::printf("qp_p50=%d\n", qp_p50);
    std::printf("qp_p90=%d\n", qp_p90);
    std::printf("qp_p95=%d\n", qp_p95);
    std::printf("fps_out_avg=%.6f\n", fps_out_avg);
    std::printf("latency_ms_p95_avg=%.6f\n", latency_ms_p95_avg);
    std::printf("latency_ms_p99_avg=%.6f\n", latency_ms_p99_avg);
    std::printf("max_input_queue_depth=%u\n", max_input_queue_depth);
    std::printf("max_output_queue_depth=%u\n", max_output_queue_depth);
    std::printf("high_qp_block_ratio=%.6f\n", high_qp_block_ratio);
    std::printf("background_block_ratio=%.6f\n", background_block_ratio);
    std::printf("flat_background_block_ratio=%.6f\n", flat_background_block_ratio);
    std::printf("roi_block_ratio=%.6f\n", roi_block_ratio);
    std::printf("ffprobe_hevc_ok=%d\n", ffprobe_ok);
    std::printf("ffprobe_bad=%d\n", ffprobe_bad);
    std::printf("errors=%" PRIu64 "\n", total_errors);
    return total_errors == 0 && failed_routes == 0 && outputs > 0 ? 0 : 1;
}

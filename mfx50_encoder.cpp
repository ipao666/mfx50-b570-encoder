#include "mfx50_transcoder.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <string>
#include <vector>

#ifdef __unix__
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

static thread_local std::string g_last_error;

struct MFX50_Context {
    MFX50_Config cfg{};
    std::vector<std::string> device_for_route;
    std::vector<MFX50_RouteStats> routes;
    MFX50_Stats stats{};
    std::string last_error;
};

struct TranscodeInput {
    std::string codec;
    std::string path;
};

static void set_global_error(const std::string& msg) { g_last_error = msg; }

static void set_error(MFX50_Context* ctx, const std::string& msg) {
    if (ctx) ctx->last_error = msg;
    set_global_error(msg);
}

static void copy_cstr(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    std::snprintf(dst, cap, "%s", src.c_str());
}

static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string par_token(const fs::path& p) {
    std::string s = p.string();
    if (s.find_first_of(" \t\n\r\"'") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static double target_fps(const MFX50_Config& cfg) {
    int num = cfg.fps_num > 0 ? cfg.fps_num : 30;
    int den = cfg.fps_den > 0 ? cfg.fps_den : 1;
    return static_cast<double>(num) / static_cast<double>(den);
}

static std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool ends_with(const std::string& s, const char* suffix) {
    const std::string needle(suffix);
    return s.size() >= needle.size() &&
           s.compare(s.size() - needle.size(), needle.size(), needle) == 0;
}

static std::string infer_codec_from_path(const std::string& path) {
    const std::string lowered = lower_copy(path);
    if (ends_with(lowered, ".h265") ||
        ends_with(lowered, ".hevc") ||
        ends_with(lowered, ".265")) {
        return "h265";
    }
    return "h264";
}

static bool parse_input_spec(const std::string& spec, TranscodeInput& out, std::string& err) {
    std::string value = trim_copy(spec);
    if (value.empty()) {
        err = "empty input";
        return false;
    }

    const size_t colon = value.find(':');
    if (colon != std::string::npos) {
        const std::string prefix = lower_copy(value.substr(0, colon));
        const std::string rest = trim_copy(value.substr(colon + 1));
        if (prefix == "h264" || prefix == "avc") {
            if (rest.empty()) {
                err = "empty h264 input path";
                return false;
            }
            out = {"h264", rest};
            return true;
        }
        if (prefix == "h265" || prefix == "hevc") {
            if (rest.empty()) {
                err = "empty h265 input path";
                return false;
            }
            out = {"h265", rest};
            return true;
        }
        if (prefix == "auto") {
            if (rest.empty()) {
                err = "empty auto input path";
                return false;
            }
            out = {infer_codec_from_path(rest), rest};
            return true;
        }
    }

    out = {infer_codec_from_path(value), value};
    return true;
}

static std::vector<TranscodeInput> read_inputs(const fs::path& p, std::string& err) {
    std::ifstream in(p);
    std::vector<TranscodeInput> inputs;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        TranscodeInput input{};
        if (!parse_input_spec(line, input, err)) return {};
        inputs.push_back(input);
    }
    return inputs;
}

static bool normalize_config(const MFX50_Config* in, MFX50_Config& out, std::string& err) {
    MFX50_DefaultConfig(&out);
    if (in) {
        int default_device_count = out.device_count;
        MFX50_DeviceRoute default_devices[MFX50_MAX_DEVICES] = {};
        for (int i = 0; i < MFX50_MAX_DEVICES; ++i) default_devices[i] = out.devices[i];
        out = *in;
        if (out.fps_num <= 0) out.fps_num = 30;
        if (out.fps_den <= 0) out.fps_den = 1;
        if (out.initial_qp <= 0) out.initial_qp = 32;
        if (out.initial_gop <= 0) out.initial_gop = 60;
        if (out.async_depth <= 0) out.async_depth = 2;
        if (out.frames_per_route <= 0) out.frames_per_route = 1000;
        if (out.device_count <= 0) {
            out.device_count = default_device_count;
            for (int i = 0; i < MFX50_MAX_DEVICES; ++i) out.devices[i] = default_devices[i];
        }
    }
    if (out.route_count <= 0) {
        err = "route_count must be > 0";
        return false;
    }
    if (out.device_count <= 0 || out.device_count > MFX50_MAX_DEVICES) {
        err = "device_count must be between 1 and MFX50_MAX_DEVICES";
        return false;
    }
    int sum = 0;
    for (int i = 0; i < out.device_count; ++i) {
        if (!out.devices[i].device_path || !out.devices[i].device_path[0]) {
            err = "device_path cannot be empty";
            return false;
        }
        if (out.devices[i].route_count < 0) {
            err = "device route_count cannot be negative";
            return false;
        }
        sum += out.devices[i].route_count;
    }
    if (sum != out.route_count) {
        err = "sum(devices[].route_count) must equal route_count";
        return false;
    }
    return true;
}

static void init_route_devices(MFX50_Context* ctx) {
    ctx->device_for_route.clear();
    for (int d = 0; d < ctx->cfg.device_count; ++d) {
        for (int i = 0; i < ctx->cfg.devices[d].route_count; ++i) {
            ctx->device_for_route.emplace_back(ctx->cfg.devices[d].device_path);
        }
    }
    ctx->routes.assign(ctx->cfg.route_count, {});
    for (int i = 0; i < ctx->cfg.route_count; ++i) {
        ctx->routes[i].route_id = i;
        ctx->routes[i].passed = 0;
        copy_cstr(ctx->routes[i].device_path, MFX50_MAX_PATH, ctx->device_for_route[i]);
    }
}

static bool make_par_file(
    MFX50_Context* ctx,
    const std::vector<TranscodeInput>& inputs,
    const fs::path& out_dir,
    fs::path& par_path,
    fs::path& log_path
) {
    fs::create_directories(out_dir);
    fs::path encoded_dir = out_dir / "encoded";
    if (ctx->cfg.write_outputs) fs::create_directories(encoded_dir);
    par_path = out_dir / "mfx50_routes.par";
    log_path = out_dir / "run.log";

    std::ofstream par(par_path);
    if (!par) {
        set_error(ctx, "cannot create par file: " + par_path.string());
        return false;
    }

    double fps = target_fps(ctx->cfg);
    for (int i = 0; i < ctx->cfg.route_count; ++i) {
        std::ostringstream name;
        name << "channel_" << std::setw(2) << std::setfill('0') << (i + 1)
             << "_qp" << ctx->cfg.initial_qp << ".hevc";
        fs::path output = ctx->cfg.write_outputs ? encoded_dir / name.str() : fs::path("null");
        par << "-hw"
            << " -device " << par_token(ctx->device_for_route[i])
            << " -i::" << inputs[i].codec << " " << par_token(inputs[i].path)
            << " -o::h265 " << par_token(output)
            << " -n " << ctx->cfg.frames_per_route
            << " -u veryfast"
            << " -async " << ctx->cfg.async_depth
            << " -MemType::video"
            << " -gpucopy::on"
            << " -cqp -qpi " << ctx->cfg.initial_qp
            << " -qpp " << ctx->cfg.initial_qp
            << " -qpb " << ctx->cfg.initial_qp
            << " -gop_size " << ctx->cfg.initial_gop
            << " -dist 1"
            << " -override_encoder_framerate " << std::fixed << std::setprecision(3) << fps
            << " -exactNframe 1"
            << " -AdaptiveI:on"
            << '\n';
    }
    return true;
}

static int decode_system_rc(int rc) {
#ifdef __unix__
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
#endif
    return rc;
}

static bool parse_log(MFX50_Context* ctx, const fs::path& log_path, const fs::path& par_path) {
    std::ifstream in(log_path);
    if (!in) {
        set_error(ctx, "cannot read run log: " + log_path.string());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::regex common_re(R"(Common transcoding time is\s+([0-9]+(?:\.[0-9]+)?)\s+sec)");
    std::smatch common_match;
    double common_time = 0.0;
    if (std::regex_search(text, common_match, common_re)) common_time = std::stod(common_match[1].str());

    std::regex session_re(
        R"(\*\*\* session\s+(\d+).*?PASSED.*?([0-9]+(?:\.[0-9]+)?)\s+sec,\s+([0-9]+)\s+frames,\s+([0-9]+(?:\.[0-9]+)?)\s+fps)"
    );
    auto begin = std::sregex_iterator(text.begin(), text.end(), session_re);
    auto end = std::sregex_iterator();
    int idx = 0;
    for (auto it = begin; it != end && idx < ctx->cfg.route_count; ++it, ++idx) {
        auto m = *it;
        auto& r = ctx->routes[idx];
        r.route_id = idx;
        r.passed = 1;
        r.seconds = std::stod(m[2].str());
        r.frames = std::stoi(m[3].str());
        r.fps = std::stod(m[4].str());
        copy_cstr(r.device_path, MFX50_MAX_PATH, ctx->device_for_route[idx]);
    }

    MFX50_Stats st{};
    st.requested_routes = ctx->cfg.route_count;
    st.frames_per_route = ctx->cfg.frames_per_route;
    st.target_fps = target_fps(ctx->cfg);
    st.common_time_sec = common_time;
    st.min_route_fps = 1e30;

    double sum = 0.0;
    for (const auto& r : ctx->routes) {
        if (r.passed) {
            st.completed_routes++;
            st.min_route_fps = std::min(st.min_route_fps, r.fps);
            st.max_route_fps = std::max(st.max_route_fps, r.fps);
            sum += r.fps;
            if (r.fps + 1e-6 < st.target_fps) st.routes_below_target_fps++;
        } else {
            st.routes_below_target_fps++;
            st.min_route_fps = 0.0;
        }
    }
    if (st.completed_routes > 0) st.avg_route_fps = sum / st.completed_routes;
    if (st.min_route_fps == 1e30) st.min_route_fps = 0.0;
    st.all_routes_realtime = (st.completed_routes == st.requested_routes && st.routes_below_target_fps == 0) ? 1 : 0;
    if (common_time > 0) st.aggregate_fps = static_cast<double>(ctx->cfg.route_count * ctx->cfg.frames_per_route) / common_time;
    copy_cstr(st.log_path, MFX50_MAX_PATH, log_path.string());
    copy_cstr(st.par_path, MFX50_MAX_PATH, par_path.string());
    ctx->stats = st;

    fs::path summary_path = log_path.parent_path() / "mfx50_summary.json";
    copy_cstr(ctx->stats.summary_path, MFX50_MAX_PATH, summary_path.string());
    std::ofstream js(summary_path);
    if (js) {
        js << "{\n";
        js << "  \"requested_routes\": " << st.requested_routes << ",\n";
        js << "  \"completed_routes\": " << st.completed_routes << ",\n";
        js << "  \"target_fps\": " << st.target_fps << ",\n";
        js << "  \"all_routes_realtime\": " << (st.all_routes_realtime ? "true" : "false") << ",\n";
        js << "  \"routes_below_target_fps\": " << st.routes_below_target_fps << ",\n";
        js << "  \"min_route_fps\": " << st.min_route_fps << ",\n";
        js << "  \"avg_route_fps\": " << st.avg_route_fps << ",\n";
        js << "  \"max_route_fps\": " << st.max_route_fps << ",\n";
        js << "  \"common_time_sec\": " << st.common_time_sec << ",\n";
        js << "  \"aggregate_fps_diagnostic\": " << st.aggregate_fps << ",\n";
        js << "  \"routes\": [\n";
        for (size_t i = 0; i < ctx->routes.size(); ++i) {
            const auto& r = ctx->routes[i];
            js << "    {\"route_id\": " << r.route_id
               << ", \"passed\": " << (r.passed ? "true" : "false")
               << ", \"frames\": " << r.frames
               << ", \"seconds\": " << r.seconds
               << ", \"fps\": " << r.fps
               << ", \"device\": \"" << r.device_path << "\"}";
            if (i + 1 < ctx->routes.size()) js << ',';
            js << '\n';
        }
        js << "  ]\n";
        js << "}\n";
    }
    return true;
}

static int run_with_inputs(MFX50_Context* ctx, const std::vector<TranscodeInput>& inputs, const char* output_dir) {
    if (!ctx) return -1;
    if (static_cast<int>(inputs.size()) < ctx->cfg.route_count) {
        set_error(ctx, "not enough inputs for requested route_count");
        return -2;
    }
    if (!output_dir || !output_dir[0]) {
        set_error(ctx, "output_dir cannot be empty");
        return -3;
    }

    fs::path out_dir(output_dir);
    fs::path par_path;
    fs::path log_path;
    auto selected = inputs;
    selected.resize(ctx->cfg.route_count);
    if (!make_par_file(ctx, selected, out_dir, par_path, log_path)) return -4;

    auto start = std::chrono::steady_clock::now();
    std::string sample = (ctx->cfg.sample_path && ctx->cfg.sample_path[0]) ? ctx->cfg.sample_path : "sample_multi_transcode";
    std::string cmd = shell_quote(sample) + " -par " + shell_quote(par_path.string()) + " > " + shell_quote(log_path.string()) + " 2>&1";
    int rc = decode_system_rc(std::system(cmd.c_str()));
    auto end = std::chrono::steady_clock::now();
    ctx->stats.wall_seconds = std::chrono::duration<double>(end - start).count();
    if (rc != 0) {
        parse_log(ctx, log_path, par_path);
        set_error(ctx, "sample_multi_transcode failed, rc=" + std::to_string(rc) + ", log=" + log_path.string());
        return rc > 0 ? -rc : -5;
    }
    if (!parse_log(ctx, log_path, par_path)) return -6;
    ctx->stats.wall_seconds = std::chrono::duration<double>(end - start).count();
    return ctx->stats.all_routes_realtime ? 0 : 1;
}

MFX50_API const char* MFX50_GetVersion(void) { return MFX50_VERSION; }

MFX50_API int MFX50_DefaultConfig(MFX50_Config* config) {
    if (!config) return -1;
    std::memset(config, 0, sizeof(*config));
    config->route_count = 50;
    config->frames_per_route = 1000;
    config->fps_num = 30;
    config->fps_den = 1;
    config->initial_qp = 32;
    config->initial_gop = 60;
    config->async_depth = 2;
    config->device_count = 2;
    config->devices[0].device_path = "/dev/dri/renderD129";
    config->devices[0].route_count = 44;
    config->devices[1].device_path = "/dev/dri/renderD128";
    config->devices[1].route_count = 6;
    config->write_outputs = 0;
    config->sample_path = nullptr;
    config->enable_internal_roi = 1;
    config->enable_quality_guard = 1;
    config->enable_motion_idr = 1;
    return 0;
}

MFX50_API MFX50_Handle MFX50_Create(const MFX50_Config* config) {
    auto* ctx = new MFX50_Context();
    std::string err;
    if (!normalize_config(config, ctx->cfg, err)) {
        set_error(ctx, err);
        g_last_error = err;
        delete ctx;
        return nullptr;
    }
    init_route_devices(ctx);
    return ctx;
}

MFX50_API int MFX50_RunInputList(MFX50_Handle h, const char* input_list_path, const char* output_dir) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    if (!ctx) return -1;
    if (!input_list_path || !input_list_path[0]) {
        set_error(ctx, "input_list_path cannot be empty");
        return -2;
    }
    std::string err;
    auto inputs = read_inputs(input_list_path, err);
    if (!err.empty()) {
        set_error(ctx, err);
        return -3;
    }
    if (inputs.empty()) {
        set_error(ctx, "input list is empty: " + std::string(input_list_path));
        return -3;
    }
    return run_with_inputs(ctx, inputs, output_dir);
}

MFX50_API int MFX50_RunSingleInput(MFX50_Handle h, const char* input_path, const char* output_dir) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    if (!ctx) return -1;
    if (!input_path || !input_path[0]) {
        set_error(ctx, "input_path cannot be empty");
        return -2;
    }
    std::string err;
    TranscodeInput input{};
    if (!parse_input_spec(input_path, input, err)) {
        set_error(ctx, err);
        return -3;
    }
    std::vector<TranscodeInput> inputs(ctx->cfg.route_count, input);
    return run_with_inputs(ctx, inputs, output_dir);
}

MFX50_API int MFX50_GetStats(MFX50_Handle h, MFX50_Stats* stats) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    if (!ctx || !stats) return -1;
    *stats = ctx->stats;
    return 0;
}

MFX50_API int MFX50_GetRouteStats(MFX50_Handle h, int route_index, MFX50_RouteStats* stats) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    if (!ctx || !stats) return -1;
    if (route_index < 0 || route_index >= static_cast<int>(ctx->routes.size())) return -2;
    *stats = ctx->routes[route_index];
    return 0;
}

MFX50_API const char* MFX50_GetLastError(MFX50_Handle h) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    if (ctx) return ctx->last_error.c_str();
    return g_last_error.c_str();
}

MFX50_API void MFX50_Close(MFX50_Handle h) {
    auto* ctx = reinterpret_cast<MFX50_Context*>(h);
    delete ctx;
}

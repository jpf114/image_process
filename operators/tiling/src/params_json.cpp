// TilingParams ↔ JSON 序列化（见 tiling/tiling_params_json.h）
#include "tiling/tiling_params_json.h"

namespace iptl {

void tilingParamsToJson(const TilingParams& p, ip::Json& out) {
    out = ip::Json::object();
    out["input"] = p.input;
    out["output_dir"] = p.output_dir;
    out["tiling_scheme"] = p.tiling_scheme;
    out["convention"] = p.convention;
    out["min_zoom"] = p.min_zoom;
    out["max_zoom"] = p.max_zoom;
    out["tile_size"] = p.tile_size;
    out["format"] = p.format;
    out["jpeg_quality"] = p.jpeg_quality;
    out["webp_quality"] = p.webp_quality;
    out["resampling"] = p.resampling;
    out["nodata_mode"] = p.nodata_mode;
    out["fill_value"] = p.fill_value;
    out["nodata_pct_threshold"] = p.nodata_pct_threshold;
    out["skip_empty_tiles"] = p.skip_empty_tiles;
    out["num_threads"] = p.num_threads;
    out["num_processes"] = p.num_processes;
    out["cache_mb"] = p.cache_mb;
    out["tile_buffer"] = p.tile_buffer;
    out["min_mem_gb"] = p.min_mem_gb;
    out["keep_tmp_vrt"] = p.keep_tmp_vrt;
    out["build_overviews"] = p.build_overviews;
    out["bbox"] = p.bbox;
    out["resume"] = p.resume;
    out["overwrite"] = p.overwrite;
    out["use_gpu"] = p.use_gpu;
    out["package"] = p.package;
    out["package_file"] = p.package_file;
}

bool tilingParamsFromJson(const ip::Json& in, TilingParams& p, std::string& err) {
    try {
        if (in.contains("input")) p.input = in["input"].get<std::string>();
        if (in.contains("output_dir"))
            p.output_dir = in["output_dir"].get<std::string>();
        if (in.contains("tiling_scheme"))
            p.tiling_scheme = in["tiling_scheme"].get<std::string>();
        if (in.contains("convention")) p.convention = in["convention"].get<std::string>();
        if (in.contains("min_zoom")) p.min_zoom = in["min_zoom"].get<int>();
        if (in.contains("max_zoom")) p.max_zoom = in["max_zoom"].get<int>();
        if (in.contains("tile_size")) p.tile_size = in["tile_size"].get<int>();
        if (in.contains("format")) p.format = in["format"].get<std::string>();
        if (in.contains("jpeg_quality")) p.jpeg_quality = in["jpeg_quality"].get<int>();
        if (in.contains("webp_quality")) p.webp_quality = in["webp_quality"].get<int>();
        if (in.contains("resampling")) p.resampling = in["resampling"].get<std::string>();
        if (in.contains("nodata_mode"))
            p.nodata_mode = in["nodata_mode"].get<std::string>();
        if (in.contains("fill_value")) p.fill_value = in["fill_value"].get<int>();
        if (in.contains("nodata_pct_threshold"))
            p.nodata_pct_threshold = in["nodata_pct_threshold"].get<int>();
        if (in.contains("skip_empty_tiles"))
            p.skip_empty_tiles = in["skip_empty_tiles"].get<bool>();
        if (in.contains("num_threads")) p.num_threads = in["num_threads"].get<int>();
        if (in.contains("num_processes"))
            p.num_processes = in["num_processes"].get<int>();
        if (in.contains("cache_mb")) p.cache_mb = in["cache_mb"].get<double>();
        if (in.contains("tile_buffer")) p.tile_buffer = in["tile_buffer"].get<int>();
        if (in.contains("min_mem_gb")) p.min_mem_gb = in["min_mem_gb"].get<double>();
        if (in.contains("keep_tmp_vrt"))
            p.keep_tmp_vrt = in["keep_tmp_vrt"].get<bool>();
        if (in.contains("build_overviews"))
            p.build_overviews = in["build_overviews"].get<std::string>();
        // 等价别名（方案 §5.2）：overview_policy 覆盖 build_overviews
        if (in.contains("overview_policy"))
            p.build_overviews = in["overview_policy"].get<std::string>();
        if (in.contains("bbox")) p.bbox = in["bbox"].get<std::string>();
        if (in.contains("resume")) p.resume = in["resume"].get<bool>();
        if (in.contains("overwrite")) p.overwrite = in["overwrite"].get<bool>();
        if (in.contains("use_gpu")) p.use_gpu = in["use_gpu"].get<bool>();
        if (in.contains("package")) p.package = in["package"].get<std::string>();
        if (in.contains("package_file"))
            p.package_file = in["package_file"].get<std::string>();
        return true;
    } catch (const std::exception& e) {
        err = std::string("tiling 参数类型错误: ") + e.what();
        return false;
    }
}

} // namespace iptl

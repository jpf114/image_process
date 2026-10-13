// VRT 最小复现（临时诊断脚本）：二分定位 GDALClose 段错误
#include "gdal_priv.h"
#include "cpl_conv.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static CPLErr pf(void** papoSources, int nSources, void* pData, int nXSize,
                 int nYSize, GDALDataType st, GDALDataType bt, int nb, int r,
                 const char* const* args) {
    if (nSources != 1 || nb != 1 || st != GDT_Byte || bt != GDT_Byte)
        return CE_Failure;
    const int rowW = nXSize + 2 * r;
    const uint8_t* s = static_cast<const uint8_t*>(papoSources[0]);
    uint8_t* d = static_cast<uint8_t*>(pData);
    fprintf(stderr, "[pf] size=%dx%d radius=%d srcPtr==dstPtr=%d\n",
            nXSize, nYSize, r, papoSources[0] == pData);
    fprintf(stderr, "[pf] s[0..7] = %d %d %d %d %d %d %d %d\n", s[0], s[1],
            s[2], s[3], s[4], s[5], s[6], s[7]);
    fprintf(stderr, "[pf] s[64..71] = %d %d %d %d %d %d %d %d\n", s[64], s[65],
            s[66], s[67], s[68], s[69], s[70], s[71]);
    fprintf(stderr, "[pf] s[192..199] = %d %d %d %d %d %d %d %d\n", s[192],
            s[193], s[194], s[195], s[196], s[197], s[198], s[199]);
    // 输出：假设输出缓冲行宽 = nXSize（无边框），直接透传源（等恒等）
    for (int y = 0; y < nYSize; ++y) {
        const uint8_t* sr = s + (size_t)y * nXSize;  // 假设：源缓冲 = 原始请求窗，行宽 nXSize
        uint8_t* dr = d + (size_t)y * nXSize;
        for (int x = 0; x < nXSize; ++x) dr[x] = sr[x];
    }
    return CE_None;
}

static void makeVrt(const char* path, const char* srcRef, bool withAlpha,
                    bool derived) {
    std::ofstream f(path);
    f << "<VRTDataset rasterXSize=\"13912\" rasterYSize=\"9279\">\n";
    int nbMax = withAlpha ? 4 : 3;
    for (int b = 1; b <= nbMax; ++b) {
        f << "  <VRTRasterBand dataType=\"Byte\" band=\"" << b << "\"";
        if (b <= 3 && derived) f << " subClass=\"VRTDerivedRasterBand\"";
        f << ">\n    <ColorInterp>"
          << (b == 1 ? "Red" : b == 2 ? "Green" : b == 3 ? "Blue" : "Alpha")
          << "</ColorInterp>\n";
        if (b <= 3 && derived)
            f << "    <PixelFunctionType>ip_gain_offset</PixelFunctionType>\n"
                 "    <PixelFunctionArguments gain=\"1\" offset=\"0\"/>\n";
        f << "    <SimpleSource>\n"
             "      <SourceFilename relativeToVRT=\"0\">" << srcRef
          << "</SourceFilename>\n"
             "      <SourceBand>" << b << "</SourceBand>\n"
             "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"13912\" ySize=\"9279\"/>\n"
             "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"13912\" ySize=\"9279\"/>\n"
             "    </SimpleSource>\n"
             "  </VRTRasterBand>\n";
    }
    f << "  <GeoTransform>105.2498, 1.8e-05, 0, 29.1668, 0, -1.8e-05</GeoTransform>\n"
      << "</VRTDataset>\n";
}

int main(int argc, char** argv) {
    GDALAllRegister();
    GDALAddDerivedBandPixelFuncWithArgs("ip_gain_offset", pf, nullptr);

    const char* srcAbs =
        "/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/"
        "data/2024/H48E018014C.tif";
    const char* srcRel =
        "文档资料/关于栅格切片和匀色/data/2024/H48E018014C.tif";

    struct Variant {
        const char* name;
        const char* src;
        bool alpha;
        bool derived;
    };
    Variant variants[] = {
        {"A_abs_3band", srcAbs, false, true},
        {"B_abs_4band", srcAbs, true, true},
        {"C_rel_3band", srcRel, false, true},
        {"D_abs_noderived", srcAbs, true, false},
    };
    int only = argc > 1 ? std::atoi(argv[1]) : -1;
    for (int vi = 0; vi < 4; ++vi) {
        if (only >= 0 && vi != only) continue;
        const Variant& v = variants[vi];
        std::string vrt = std::string("/tmp/vrt_") + v.name + ".vrt";
        makeVrt(vrt.c_str(), v.src, v.alpha, v.derived);
        GDALDataset* ds = GDALDataset::Open(
            vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY);
        if (!ds) {
            printf("[%s] OPEN FAIL\n", v.name);
            fflush(stdout);
            continue;
        }
        uint8_t buf[64 * 64];
        CPLErr e = ds->GetRasterBand(1)->RasterIO(GF_Read, 100, 100, 64, 64,
                                                  buf, 64, 64, GDT_Byte, 0, 0,
                                                  nullptr);
        printf("[%s] read=%d v0=%d\n", v.name, (int)e, buf[0]);
        // 源邻域 5x5 网格，定位 VRT 值 117 对应的源位置（几何偏移检测）
        GDALDataset* sd = GDALDataset::Open(v.src, GDAL_OF_RASTER | GDAL_OF_READONLY);
        if (sd) {
            uint8_t nb[5][5];
            // 对照：源 (36,36) 与 (100,100) 的 3x3（判定 pf 源缓冲的窗口起点）
            uint8_t w36[3][3], w100[3][3];
            sd->GetRasterBand(1)->RasterIO(GF_Read, 36, 36, 3, 3, w36, 3, 3, GDT_Byte, 0, 0, nullptr);
            sd->GetRasterBand(1)->RasterIO(GF_Read, 100, 100, 3, 3, w100, 3, 3, GDT_Byte, 0, 0, nullptr);
            printf("[%s] src(36,36)3x3: %d %d %d / %d %d %d / %d %d %d\n", v.name,
                   w36[0][0], w36[0][1], w36[0][2], w36[1][0], w36[1][1], w36[1][2], w36[2][0], w36[2][1], w36[2][2]);
            printf("[%s] src(100,100)3x3: %d %d %d / %d %d %d / %d %d %d\n", v.name,
                   w100[0][0], w100[0][1], w100[0][2], w100[1][0], w100[1][1], w100[1][2], w100[2][0], w100[2][1], w100[2][2]);
            GDALClose(sd);
        }
        fflush(stdout);
        GDALClose(ds);
        printf("[%s] closed OK\n", v.name);
        fflush(stdout);
    }
    return 0;
}

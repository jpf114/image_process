// 诊断：匀色 VRT 管线各级亮度（同进程注册像素函数后逐层对比）
#include "color_balance/i_color_balance.h"

#include "ip/gdal_raii.h"
#include "ip/log.h"

#include <cstdio>

using namespace ip;

static void stats(const char* tag, GDALDataset* ds, int x0, int y0, int w, int h) {
    if (!ds) {
        printf("%-28s: OPEN FAIL\n", tag);
        return;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(ds->GetRasterCount()) * w * h);
    for (int b = 0; b < ds->GetRasterCount(); ++b) {
        ds->GetRasterBand(b + 1)->RasterIO(
            GF_Read, x0, y0, w, h, buf.data() + b * w * h, w, h, GDT_Byte, 0, 0, nullptr);
    }
    double acc[3] = {0, 0, 0};
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        for (int b = 0; b < 3; ++b) acc[b] += buf[b * n + i];
    }
    printf("%-28s: mean = %.1f / %.1f / %.1f\n", tag, acc[0] / n, acc[1] / n,
           acc[2] / n);
}

int main(int argc, char** argv) {
    // argv: <原始景> <匀色景VRT> <mosaic.vrt> <原始镶嵌vrt> <mosaicX> <mosaicY>
    GDALAllRegister();
    ensureColorBalancePixelFunc();

    const char* src = argv[1];
    const char* bal = argv[2];
    const char* mos = argv[3];
    const char* ref = argv[4];
    const int mx = std::atoi(argv[5]);
    const int my = std::atoi(argv[6]);

    auto dsSrc = openReadOnly(src);
    auto dsBal = openReadOnly(bal);
    auto dsMos = openReadOnly(mos);
    auto dsRef = openReadOnly(ref);
    stats("原始景(0,0,512,512)", dsSrc.get(), 0, 0, 512, 512);
    stats("匀色VRT(0,0,512,512)", dsBal.get(), 0, 0, 512, 512);
    stats("mosaic.vrt(0,0,512,512)", dsMos.get(), 0, 0, 512, 512);
    stats("ref mosaic(0,0,512,512)", dsRef.get(), 0, 0, 512, 512);
    stats("mosaic.vrt(窗@中心)", dsMos.get(), mx, my, 512, 512);
    stats("ref mosaic(窗@中心)", dsRef.get(), mx, my, 512, 512);
    stats("mosaic.vrt(14300,4000)", dsMos.get(), 14300, 4000, 512, 512);
    stats("ref mosaic(14300,4000)", dsRef.get(), 14300, 4000, 512, 512);
    stats("mosaic.vrt(13950,4000)", dsMos.get(), 13950, 4000, 512, 512);
    stats("ref mosaic(13950,4000)", dsRef.get(), 13950, 4000, 512, 512);
    stats("mosaic.vrt(14300,4000)", dsMos.get(), 14300, 4000, 512, 512);
    stats("ref mosaic(14300,4000)", dsRef.get(), 14300, 4000, 512, 512);
    stats("mosaic.vrt(13950,4000)", dsMos.get(), 13950, 4000, 512, 512);
    stats("ref mosaic(13950,4000)", dsRef.get(), 13950, 4000, 512, 512);
    return 0;
}

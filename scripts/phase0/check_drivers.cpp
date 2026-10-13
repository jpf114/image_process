// 检查 vcpkg GDAL 的编码驱动可用性（PNG/JPEG/WEBP/MEM/VRT/WarpedVRT）
#include "gdal_priv.h"
#include <cstdio>

int main() {
    GDALAllRegister();
    const char* drivers[] = {"PNG", "JPEG", "WEBP", "MEM", "VRT", "GTiff", "GDAL_VRT"};
    for (const char* n : drivers) {
        GDALDriver* d = GetGDALDriverManager()->GetDriverByName(n);
        printf("%-10s: %s\n", n, d ? "OK" : "MISSING");
    }
    // WEBP Create 冒烟（RGBA 4x4）
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* ds = mem->Create("", 4, 4, 4, GDT_Byte, nullptr);
    GDALDriver* webp = GetGDALDriverManager()->GetDriverByName("WEBP");
    char** opts = CSLSetNameValue(nullptr, "QUALITY", "85");
    GDALDataset* out = webp->CreateCopy("/tmp/_smoke.webp", ds, false, opts, nullptr, nullptr);
    CSLDestroy(opts);
    printf("WEBP CreateCopy: %s\n", out ? "OK" : "FAIL");
    if (out) GDALClose(out);
    GDALClose(ds);
    return 0;
}

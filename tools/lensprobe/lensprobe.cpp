// lensprobe: what the SR SDK's weaving library (Dimenco::Weaver) reports for the
// lens, under different ways of telling it which display to load. No window, no
// weaving, numbers only. Usage:
//   lensprobe                      as it is (the library's defaults)
//   lensprobe ini <path>           SetGlobalParameters(<path>, position)
//   lensprobe dev <serial> <path>  SetDeviceSerialNumbers + SetInstallPath + ReconfigureWeaver
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "sr/weaver/Weaver.h"

static bool Report(const char* what)
{
    __try
    {
        printf("%s: slant %.6f, pitch %.6f px, N %.4f, D/N %.4f mm, crosstalk %.4f / %.4f, pattern %.0f, filter %.3f / %.3f\n", what,
               Dimenco::Weaver::GetSlant(), Dimenco::Weaver::GetPx(), Dimenco::Weaver::GetN(), Dimenco::Weaver::GetDoN(),
               Dimenco::Weaver::GetXTalkFactor(), Dimenco::Weaver::GetXTalkDynamicFactor(), Dimenco::Weaver::GetPattern(),
               Dimenco::Weaver::GetFilterWidth(), Dimenco::Weaver::GetFilterSlope());
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("%s: reading threw\n", what); return false; }
}
static float g_px = 0.0f, g_py = 0.0f, g_pz = 600.0f;
static void Attributes()
{
    __try
    {
        for (int k = 0; k < 2; ++k)
        {
            FLOAT4 ph, dxy; FLOAT2 sp, wv;
            Dimenco::Weaver::FillAttributes(FLOAT2(3840.0f, 2160.0f), k == 0 ? FLOAT4(-1.0f, 1.0f, 0.0f, 1.0f) : FLOAT4(1.0f, -1.0f, 0.0f, 1.0f), ph, dxy, sp, wv);
            printf("  attributes at %s: phases %.5f %.5f %.5f | dxy %.7f %.7f %.7f %.7f | vars %.5f %.5f\n", k == 0 ? "top-left" : "bottom-right",
                   ph.x, ph.y, ph.z, dxy.x, dxy.y, dxy.z, dxy.w, wv.x, wv.y);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("  attributes: threw\n"); }
}
static void Configure(int argc, char** argv)
{
    try
    {
        if (argc >= 3 && !strcmp(argv[1], "ini"))
            Dimenco::Weaver::SetGlobalParameters(argv[2], FLOAT3(g_px, g_py, g_pz));
        else if (argc >= 4 && !strcmp(argv[1], "dev"))
        {
            Dimenco::Weaver::SetInstallPath(std::string(argv[3]));
            Dimenco::Weaver::SetDeviceSerialNumbers(std::vector<std::string>{ std::string(argv[2]) });
            Dimenco::Weaver::ReconfigureWeaver();
            Dimenco::Weaver::SetGlobalParameters(FLOAT3(g_px, g_py, g_pz));
        }
        else
            Dimenco::Weaver::SetGlobalParameters(FLOAT3(g_px, g_py, g_pz));
    }
    catch (std::exception& e) { printf("configuring threw: %s\n", e.what()); }
    catch (...) { printf("configuring threw\n"); }
}
int main(int argc, char** argv)
{
    if (const char* p = getenv("LENSPROBE_POS")) sscanf(p, "%f,%f,%f", &g_px, &g_py, &g_pz);
    Report("before");
    Configure(argc, argv);
    if (Report("after ")) Attributes();
    return 0;
}

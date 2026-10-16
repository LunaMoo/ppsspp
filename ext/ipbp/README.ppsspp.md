The conversion core of iPBP (https://github.com/LunaMoo/iPBP, 1.2.0): ISO -> PSN-style EBOOT.PBP
(NPUMDIMG), used by the PBP presets in UI/PBPCompress.cpp. The C API (capi.cpp, ipbp.h), the
command line tool and the tests are left out, and so is src/io.cpp: UI/PBPCompress.cpp implements
its functions on PPSSPP's file layer instead (Android content URIs, UWP).

Changes: "ipbp/ipbp.hpp" is included by relative path, so no extra include path is needed, and
src/util.cpp is renamed ipbp_util.cpp so its object file doesn't clash with Zopfli's util.c in the
Visual Studio project. GPL 3.0, see LICENSE.

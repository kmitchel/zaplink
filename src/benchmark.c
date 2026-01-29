#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "benchmark.h"
#include "log.h"

static int test_encoder(const char *name, const char *ffmpeg_args) {
    char cmd[512];
    // Redirect all output to /dev/null for clean benchmark output
    snprintf(cmd, sizeof(cmd), 
        "ffmpeg -v quiet -f lavfi -i testsrc=duration=1:size=1280x720:rate=30 -c:v %s %s -f null - >/dev/null 2>&1", 
        name, ffmpeg_args);
    int res = system(cmd);
    return (WIFEXITED(res) && WEXITSTATUS(res) == 0);
}

void run_transcode_benchmark() {
    printf("\n");
    printf("==========================================================\n");
    printf("           ZapLink Transcoding Benchmark                  \n");
    printf("==========================================================\n\n");
    
    int h264[4], hevc[4], av1[4];
    
    printf("  Testing encoders");
    fflush(stdout);
    
    // Test H.264
    h264[0] = test_encoder("libx264", "-preset ultrafast");
    printf("."); fflush(stdout);
    h264[1] = test_encoder("h264_qsv", "");
    printf("."); fflush(stdout);
    h264[2] = test_encoder("h264_nvenc", "");
    printf("."); fflush(stdout);
    h264[3] = test_encoder("h264_vaapi", "-vaapi_device /dev/dri/renderD128 -vf 'format=nv12,hwupload'");
    printf("."); fflush(stdout);
    
    // Test HEVC
    hevc[0] = test_encoder("libx265", "-preset ultrafast");
    printf("."); fflush(stdout);
    hevc[1] = test_encoder("hevc_qsv", "");
    printf("."); fflush(stdout);
    hevc[2] = test_encoder("hevc_nvenc", "");
    printf("."); fflush(stdout);
    hevc[3] = test_encoder("hevc_vaapi", "-vaapi_device /dev/dri/renderD128 -vf 'format=nv12,hwupload'");
    printf("."); fflush(stdout);
    
    // Test AV1
    av1[0] = test_encoder("libsvtav1", "-preset 10");
    printf("."); fflush(stdout);
    av1[1] = test_encoder("av1_qsv", "");
    printf("."); fflush(stdout);
    av1[2] = test_encoder("av1_nvenc", "");
    printf("."); fflush(stdout);
    av1[3] = test_encoder("av1_vaapi", "-vaapi_device /dev/dri/renderD128 -vf 'format=nv12,hwupload'");
    printf(" done!\n\n");
    
    // Summary table
    printf("  +-----------+----------+-----------+--------+--------+\n");
    printf("  |   Codec   | Software | Intel QSV | NVENC  | VA-API |\n");
    printf("  +-----------+----------+-----------+--------+--------+\n");
    printf("  |   H.264   |   %s    |    %s    |  %s   |  %s   |\n", 
           h264[0] ? "Yes" : "No ",
           h264[1] ? "Yes" : "No ",
           h264[2] ? "Yes" : "No ",
           h264[3] ? "Yes" : "No ");
    printf("  |   HEVC    |   %s    |    %s    |  %s   |  %s   |\n",
           hevc[0] ? "Yes" : "No ",
           hevc[1] ? "Yes" : "No ",
           hevc[2] ? "Yes" : "No ",
           hevc[3] ? "Yes" : "No ");
    printf("  |   AV1     |   %s    |    %s    |  %s   |  %s   |\n",
           av1[0] ? "Yes" : "No ",
           av1[1] ? "Yes" : "No ",
           av1[2] ? "Yes" : "No ",
           av1[3] ? "Yes" : "No ");
    printf("  +-----------+----------+-----------+--------+--------+\n");
    
    // Recommendations
    printf("\n  Recommended stream parameters:\n");
    int has_hw = 0;
    if (h264[1] || hevc[1]) { printf("    Intel QuickSync: ?backend=qsv&codec=h264\n"); has_hw = 1; }
    if (h264[2] || hevc[2]) { printf("    NVIDIA NVENC:    ?backend=nvenc&codec=h264\n"); has_hw = 1; }
    if (h264[3] || hevc[3]) { printf("    VA-API:          ?backend=vaapi&codec=h264\n"); has_hw = 1; }
    if (!has_hw) printf("    No hardware acceleration detected. Use software (default).\n");
    
    printf("\n");
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include "benchmark.h"
#include "log.h"

/* Find the first usable VA-API render node, e.g. /dev/dri/renderD128 */
static const char *find_vaapi_device(void) {
    static char device[64];
    glob_t matches;
    if (glob("/dev/dri/renderD*", GLOB_NOSORT, NULL, &matches) != 0)
        return "/dev/dri/renderD128";  /* fallback */
    if (matches.gl_pathc > 0)
        snprintf(device, sizeof(device), "%s", matches.gl_pathv[0]);
    else
        snprintf(device, sizeof(device), "/dev/dri/renderD128");
    globfree(&matches);
    return device;
}

static int test_encoder(char *name, char *const extra_args[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char *args[32];
        int count = 0;
        args[count++] = "ffmpeg";
        args[count++] = "-v";
        args[count++] = "quiet";
        args[count++] = "-f";
        args[count++] = "lavfi";
        args[count++] = "-i";
        args[count++] = "testsrc=duration=1:size=1280x720:rate=30";
        args[count++] = "-c:v";
        args[count++] = name;
        for (int i = 0; extra_args && extra_args[i] && count < 27; i++) {
            args[count++] = extra_args[i];
        }
        args[count++] = "-f";
        args[count++] = "null";
        args[count++] = "-";
        args[count] = NULL;
        execvp(args[0], args);
        _exit(127);
    }
    if (pid < 0) return 0;
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

void run_transcode_benchmark() {
    static char *no_args[] = {NULL};
    static char *software_args[] = {"-preset", "ultrafast", NULL};
    static char *av1_args[] = {"-preset", "10", NULL};
    const char *vaapi_dev = find_vaapi_device();
    char *vaapi_args[] = {
        "-vaapi_device", (char *)vaapi_dev, "-vf",
        "format=nv12,hwupload", NULL
    };
    printf("\n");
    printf("==========================================================\n");
    printf("           ZapLink Transcoding Benchmark                  \n");
    printf("==========================================================\n\n");
    
    int h264[4], hevc[4], av1[4];
    
    printf("  Testing encoders");
    fflush(stdout);
    
    // Test H.264
    h264[0] = test_encoder("libx264", software_args);
    printf("."); fflush(stdout);
    h264[1] = test_encoder("h264_qsv", no_args);
    printf("."); fflush(stdout);
    h264[2] = test_encoder("h264_nvenc", no_args);
    printf("."); fflush(stdout);
    h264[3] = test_encoder("h264_vaapi", vaapi_args);
    printf("."); fflush(stdout);
    
    // Test HEVC
    hevc[0] = test_encoder("libx265", software_args);
    printf("."); fflush(stdout);
    hevc[1] = test_encoder("hevc_qsv", no_args);
    printf("."); fflush(stdout);
    hevc[2] = test_encoder("hevc_nvenc", no_args);
    printf("."); fflush(stdout);
    hevc[3] = test_encoder("hevc_vaapi", vaapi_args);
    printf("."); fflush(stdout);
    
    // Test AV1
    av1[0] = test_encoder("libsvtav1", av1_args);
    printf("."); fflush(stdout);
    av1[1] = test_encoder("av1_qsv", no_args);
    printf("."); fflush(stdout);
    av1[2] = test_encoder("av1_nvenc", no_args);
    printf("."); fflush(stdout);
    av1[3] = test_encoder("av1_vaapi", vaapi_args);
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

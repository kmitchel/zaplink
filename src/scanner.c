#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <ctype.h>
#include <glob.h>
#include <fcntl.h>
#include <errno.h>
#include "scanner.h"

#define RABBIT_EARS_URL "https://www.rabbitears.info/search.php?request=zip_search&zipcode="
#define MAX_ADAPTERS 16

typedef struct {
    int id;
    pid_t pid;
    int pipe_fd;
    char buffer[4096];  // Larger buffer for verbose dvbv5-scan output
    int buf_len;
    int overflow_warned; // Track if we've warned about overflow
} ScanWorker;

// ATSC Center Frequencies (kHz)
static int get_center_freq(int channel) {
    if (channel >= 2 && channel <= 4) return (57 + (channel - 2) * 6) * 1000000;
    if (channel >= 5 && channel <= 6) return (79 + (channel - 5) * 6) * 1000000;
    if (channel >= 7 && channel <= 13) return (177 + (channel - 7) * 6) * 1000000;
    if (channel >= 14 && channel <= 36) return (473 + (channel - 14) * 6) * 1000000;
    if (channel >= 37 && channel <= 69) return (473 + (channel - 14) * 6) * 1000000;
    return 0;
}

static int get_adapter_count() {
    glob_t g;
    if (glob("/dev/dvb/adapter*", 0, NULL, &g) != 0) {
        return 0;
    }
    int count = g.gl_pathc;
    globfree(&g);
    return count;
}

static void fetch_and_process(const char *zip, const char *scan_file, int skip_vhf) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", RABBIT_EARS_URL, zip);
    
    char tmp_html[] = "/tmp/zapcore_scan.html";
    
    printf("[SCANNER] Querying RabbitEars for %s...%s\n", zip, skip_vhf ? " (skipping VHF)" : "");
    
    /* Use fork/exec instead of system() to avoid command injection risk */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        execlp("curl", "curl", "-s", "-o", tmp_html, url, NULL);
        _exit(1);
    } else if (pid > 0) {
        /* Parent: wait for curl to complete */
        int status;
        waitpid(pid, &status, 0);
    }
    
    FILE *f = fopen(tmp_html, "r");
    if (!f) {
        printf("[SCANNER] Failed to fetch data. Falling back to full scan.\n");
        f = NULL;
    }

    int found_channels[100] = {0};
    int count = 0;

    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while ((p = strchr(p, '('))) {
                int ch = atoi(p + 1);
                // VHF channels are 2-13, UHF starts at 14
                int is_vhf = (ch >= 2 && ch <= 13);
                if (ch >= 2 && ch <= 69) {
                    if (skip_vhf && is_vhf) {
                        // Skip VHF channel
                    } else if (!found_channels[ch]) {
                        found_channels[ch] = 1;
                        count++;
                    }
                }
                p++;
            }
        }
        fclose(f);
        unlink(tmp_html);
    }
    
    if (count == 0) {
        printf("[SCANNER] Generating full US ATSC scan list...%s\n", skip_vhf ? " (UHF only)" : "");
        int start_ch = skip_vhf ? 14 : 2;
        for (int i = start_ch; i <= 36; i++) found_channels[i] = 1;
        count = 36 - start_ch + 1;
    } else {
        printf("[SCANNER] Found %d potential channels nearby.\n", count);
    }
    
    FILE *out = fopen(scan_file, "w");
    if (!out) return;
    
    for (int i = 2; i <= 69; i++) {
        if (found_channels[i]) {
            int freq = get_center_freq(i);
            fprintf(out, "[CHANNEL_%d]\n", i);
            fprintf(out, "\tDELIVERY_SYSTEM = ATSC\n");
            fprintf(out, "\tFREQUENCY = %d\n", freq);
            fprintf(out, "\tMODULATION = VSB/8\n");
            fprintf(out, "\tINVERSION = AUTO\n\n");
        }
    }
    fclose(out);
}

// Split the master scan list into N temporary files
static void split_scan_list(const char *scan_file, int parts, char part_files[][64]) {
    FILE *in = fopen(scan_file, "r");
    if (!in) return;
    
    FILE *outs[MAX_ADAPTERS];
    for (int i = 0; i < parts; i++) {
        snprintf(part_files[i], 64, "%s.part%d", scan_file, i);
        outs[i] = fopen(part_files[i], "w");
    }
    
    char line[256];
    int current_part = 0;
    // We split by blocks (CHANNEL_X ... empty line)
    while (fgets(line, sizeof(line), in)) {
        if (outs[current_part]) fputs(line, outs[current_part]);
        // If line is empty newline, it marks end of a block, switch part
        if (line[0] == '\n' || line[0] == '\r') {
            current_part = (current_part + 1) % parts;
        }
    }
    
    fclose(in);
    for (int i = 0; i < parts; i++) {
        if (outs[i]) fclose(outs[i]);
    }
}

static void parse_output_line(int tuner_id, char *line) {
    // Strip ansi
    // "Virtual channel 55.1, name = WFFT-TV"
    // "Lock   (0x1f) Signal= -39.00dBm C/N= 32.77dB"
    
    // Simple sanitization of ANSI codes could be complex, assuming raw text for now or simple skip
    // dvbv5-scan -v output is verbose
    
    char *lock = strstr(line, "Lock");
    if (lock) {
         // Filter out standard lock messages to reduce noise, or print them nicely?
         // User asked to indicate when channels are found.
         // Let's print lock only if signal is weak?
         // printf("[Tuner %d] %s", tuner_id, lock);
         return; 
    }
    
    char *vchan = strstr(line, "Virtual channel");
    if (vchan) {
        // Parse "Virtual channel 55.1, name = NAME"
        // Remove trailing newline
        line[strcspn(line, "\n")] = 0;
        printf(" [OK] [Tuner %d] Found: %s\n", tuner_id, vchan + 16); // +16 skips "Virtual channel "
    }
    
    char *scan = strstr(line, "Scanning frequency");
    if (scan) {
         // "Scanning frequency #1 57000000"
         // Extract freq
         // printf("[Tuner %d] %s", tuner_id, scan);
    }
}

static void run_parallel_scan(int num_adapters, char part_files[][64], const char *dest_file) {
    ScanWorker workers[MAX_ADAPTERS];
    memset(workers, 0, sizeof(workers));
    int active_workers = 0;
    
    printf("\n[SCANNER] Starting parallel scan on %d tuners...\n", num_adapters);
    
    for (int i = 0; i < num_adapters; i++) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("pipe");
            continue;
        }
        
        pid_t pid = fork();
        if (pid == 0) {
            // Child
            close(pipefd[0]);
            dup2(pipefd[1], STDERR_FILENO); // Capture stderr (where verbose logs go)
            dup2(pipefd[1], STDOUT_FILENO); // Capture stdout too? usually configs go to -o file
            close(pipefd[1]);
            
            char out_part[64];
            snprintf(out_part, sizeof(out_part), "%s.out", part_files[i]);
            
            char adapter_arg[16];
            snprintf(adapter_arg, sizeof(adapter_arg), "%d", i);
            
            // exec dvbv5-scan
            // dvbv5-scan -a <i> -F -T 0.5 -v -C us -o <out_part> <part_file>
            execlp("dvbv5-scan", "dvbv5-scan", 
                   "-a", adapter_arg,
                   "-F", "-T", "0.5", "-v", "-C", "us",
                   "-o", out_part,
                   part_files[i], NULL);
            exit(1);
        } else if (pid > 0) {
            // Parent
            close(pipefd[1]);
            workers[i].id = i;
            workers[i].pid = pid;
            workers[i].pipe_fd = pipefd[0];
            workers[i].buf_len = 0;
            workers[i].overflow_warned = 0;
            active_workers++;
        }
    }
    
    // Read loop
    while (active_workers > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        int max_fd = 0;
        
        for (int i = 0; i < num_adapters; i++) {
            if (workers[i].pid > 0) {
                FD_SET(workers[i].pipe_fd, &fds);
                if (workers[i].pipe_fd > max_fd) max_fd = workers[i].pipe_fd;
            }
        }
        
        struct timeval tv = {1, 0}; // 1 sec timeout
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0) {
            for (int i = 0; i < num_adapters; i++) {
                if (workers[i].pid > 0 && FD_ISSET(workers[i].pipe_fd, &fds)) {
                    char buf[256];
                    ssize_t n = read(workers[i].pipe_fd, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = 0;
                        // For simplicity, just scan for newlines in chunk. 
                        // Real parser needs buffer management.
                        // We hack it: Print line if newline found, else store?
                        // Simplified: parse what we got. 
                        // Ideally we buffer, but 'parse_output_line' using strstr is robust enough for fragments? No.
                        // Let's just print complete lines.
                        
                        // Append to worker buffer
                        if (workers[i].buf_len + n < (int)sizeof(workers[i].buffer) - 1) {
                            memcpy(workers[i].buffer + workers[i].buf_len, buf, n);
                            workers[i].buf_len += n;
                            workers[i].buffer[workers[i].buf_len] = 0;
                        } else {
                            // Buffer overflow: discard partial line and reset
                            if (!workers[i].overflow_warned) {
                                printf(" [WARN] [Tuner %d] Buffer overflow, some output may be missed\n", i);
                                workers[i].overflow_warned = 1;
                            }
                            // Find last newline and keep only what's after it
                            char *last_nl = strrchr(workers[i].buffer, '\n');
                            if (last_nl) {
                                int keep = workers[i].buf_len - (last_nl + 1 - workers[i].buffer);
                                memmove(workers[i].buffer, last_nl + 1, keep);
                                workers[i].buf_len = keep;
                            } else {
                                // No newline, discard everything
                                workers[i].buf_len = 0;
                            }
                            workers[i].buffer[workers[i].buf_len] = 0;
                            // Now append new data if it fits
                            if (workers[i].buf_len + n < (int)sizeof(workers[i].buffer) - 1) {
                                memcpy(workers[i].buffer + workers[i].buf_len, buf, n);
                                workers[i].buf_len += n;
                                workers[i].buffer[workers[i].buf_len] = 0;
                            }
                        }
                        
                        // Process complete lines
                        char *start = workers[i].buffer;
                        char *newline;
                        while ((newline = strchr(start, '\n'))) {
                            *newline = 0;
                            parse_output_line(i, start);
                            start = newline + 1;
                        }
                        
                        // Move remaining partial line to beginning
                        int remaining = workers[i].buf_len - (start - workers[i].buffer);
                        if (remaining > 0 && start != workers[i].buffer) {
                            memmove(workers[i].buffer, start, remaining);
                        }
                        workers[i].buf_len = remaining;
                        workers[i].buffer[remaining] = 0;
                        
                    } else {
                        // EOF
                        close(workers[i].pipe_fd);
                        waitpid(workers[i].pid, NULL, 0);
                        workers[i].pid = 0; // Mark done
                        active_workers--;
                    }
                }
            }
        } 
        else if (ret == 0) {
            // Timeout check if processes died?
            for (int i = 0; i < num_adapters; i++) {
                 if (workers[i].pid > 0) {
                    int status;
                    if (waitpid(workers[i].pid, &status, WNOHANG) > 0) {
                         close(workers[i].pipe_fd);
                         workers[i].pid = 0;
                         active_workers--;
                    }
                 }
            }
        }
    }
    
    // Merge
    printf("[SCANNER] Merging results...\n");
    FILE *dest = fopen(dest_file, "w");
    if (!dest) return;
    
    for (int i = 0; i < num_adapters; i++) {
        char part_out[64];
        snprintf(part_out, sizeof(part_out), "%s.out", part_files[i]);
        FILE *src = fopen(part_out, "r");
        if (src) {
            char ch;
            while ((ch = fgetc(src)) != EOF) fputc(ch, dest);
            fclose(src);
            unlink(part_out); // Cleanup
        }
        unlink(part_files[i]); // Cleanup input part
    }
    fclose(dest);
    printf("[SCANNER] Scan complete! Saved to %s\n", dest_file);
}

int scanner_check(const char *config_path) {
    if (access(config_path, F_OK) == 0) {
        return 0; 
    }
    
    printf("\n============================================\n");
    printf("        ZapLink Setup @ %s\n", config_path);
    printf("============================================\n");
    
    int adapters = get_adapter_count();
    printf("Detected %d tuners.\n", adapters);
    if (adapters == 0) {
        printf("Error: No DVB adapters found in /dev/dvb/.\n");
        return 0; // Can't scan
    }
    
    printf("No channels.conf found. Run Automatic Channel Scanner? [Y/n]: ");
    char buf[16];
    if (fgets(buf, sizeof(buf), stdin)) {
        if (buf[0] == 'n' || buf[0] == 'N') return 0;
    }
    
    // Zip Code Input
    char zip[16] = {0};
    while (1) {
        printf("Enter your Zip Code (or leave empty for full scan): ");
        if (!fgets(zip, sizeof(zip), stdin)) continue;
        zip[strcspn(zip, "\n")] = 0;
        
        if (zip[0] == '\0') break; 
        int valid = (strlen(zip) == 5);
        for (int i = 0; i < 5 && valid; i++) if (!isdigit((unsigned char)zip[i])) valid = 0;
        if (valid) break;
        printf("Invalid Zip Code. Please enter exactly 5 digits.\n");
    }
    
    // VHF Skip Option
    int skip_vhf = 0;
    printf("\nSkip VHF channels (RF 2-13)? VHF reception often requires\n");
    printf("a larger antenna and is more prone to interference. [y/N]: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        if (buf[0] == 'y' || buf[0] == 'Y') skip_vhf = 1;
    }
    
    char master_scan_file[] = "/tmp/zapcore_master_scan.conf";
    if (zip[0]) fetch_and_process(zip, master_scan_file, skip_vhf);
    else fetch_and_process("00000", master_scan_file, skip_vhf);
    
    printf("\nReady to scan. Please ensure your antenna is connected.\n");
    printf("Press ENTER to start scanning...");
    fgets(buf, sizeof(buf), stdin);
    
    char part_files[MAX_ADAPTERS][64];
    split_scan_list(master_scan_file, adapters, part_files);
    unlink(master_scan_file);
    
    run_parallel_scan(adapters, part_files, config_path);
    
    if (access(config_path, F_OK) == 0) {
        printf("\n[SUCCESS] Configuration generated! Exiting.\n");
        return 1;
    }
    return 0;
}

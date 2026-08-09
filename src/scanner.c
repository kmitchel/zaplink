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
#include <signal.h>
#include "log.h"
#include "scanner.h"
#include "config.h"

#define RABBIT_EARS_URL "https://www.rabbitears.info/search.php?request=zip_search&zipcode="
#define MAX_ADAPTERS MAX_TUNERS
#define MAX_MULTIPLEXES 128
#define MAX_SCAN_SECTIONS 512

typedef struct {
    int id;
    pid_t pid;
    int pipe_fd;
    char buffer[4096];  // Larger buffer for verbose dvbv5-scan output
    int buf_len;
    int overflow_warned; // Track if we've warned about overflow
    int wait_status;
    int status_valid;
} ScanWorker;

typedef struct {
    char frequency[32];
    char channel_name[128];
    double signal_dbm;
    double cnr_db;
    int sample_count;
    int has_lock;
    int weak;
} ScanMultiplex;

static char *skip_space(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    return text;
}

static void copy_value(char *dest, size_t dest_size, const char *value) {
    while (*value && isspace((unsigned char)*value)) value++;
    size_t length = strcspn(value, "\r\n");
    while (length > 0 && isspace((unsigned char)value[length - 1])) length--;
    if (length >= dest_size) length = dest_size - 1;
    memcpy(dest, value, length);
    dest[length] = '\0';
}

static int find_multiplex(ScanMultiplex multiplexes[], int count,
                          const char *frequency) {
    for (int i = 0; i < count; i++) {
        if (strcmp(multiplexes[i].frequency, frequency) == 0) return i;
    }
    return -1;
}

static int load_scan_multiplexes(const char *config_path,
                                 ScanMultiplex multiplexes[], int *mux_count,
                                 int section_mux[], int *section_count) {
    FILE *config = fopen(config_path, "r");
    if (!config) return 0;

    char line[512];
    char current_name[128] = "";
    int current_section = -1;
    *mux_count = 0;
    *section_count = 0;

    while (fgets(line, sizeof(line), config)) {
        char *trimmed = skip_space(line);
        size_t length = strcspn(trimmed, "\r\n");

        if (trimmed[0] == '[' && length > 2 && trimmed[length - 1] == ']') {
            if (*section_count >= MAX_SCAN_SECTIONS) {
                current_section = -1;
                continue;
            }
            current_section = (*section_count)++;
            section_mux[current_section] = -1;
            size_t name_length = length - 2;
            if (name_length >= sizeof(current_name)) name_length = sizeof(current_name) - 1;
            memcpy(current_name, trimmed + 1, name_length);
            current_name[name_length] = '\0';
            continue;
        }

        if (current_section < 0 || strncmp(trimmed, "FREQUENCY", 9) != 0) continue;
        char *equals = strchr(trimmed, '=');
        if (!equals) continue;

        char frequency[32];
        copy_value(frequency, sizeof(frequency), equals + 1);
        int mux = find_multiplex(multiplexes, *mux_count, frequency);
        if (mux < 0 && *mux_count < MAX_MULTIPLEXES) {
            mux = (*mux_count)++;
            memset(&multiplexes[mux], 0, sizeof(multiplexes[mux]));
            snprintf(multiplexes[mux].frequency,
                     sizeof(multiplexes[mux].frequency), "%s", frequency);
            snprintf(multiplexes[mux].channel_name,
                     sizeof(multiplexes[mux].channel_name), "%s", current_name);
        }
        section_mux[current_section] = mux;
    }

    fclose(config);
    return *mux_count;
}

static void parse_signal_sample(ScanMultiplex *multiplex, const char *line) {
    if (!strstr(line, "Lock")) return;

    const char *signal = strstr(line, "Signal=");
    const char *cnr = strstr(line, "C/N=");
    if (!signal || !cnr) return;

    char *signal_end;
    char *cnr_end;
    double signal_value = strtod(signal + 7, &signal_end);
    double cnr_value = strtod(cnr + 4, &cnr_end);
    if (signal_end == signal + 7 || cnr_end == cnr + 4) return;

    multiplex->signal_dbm += signal_value;
    multiplex->cnr_db += cnr_value;
    multiplex->sample_count++;
    multiplex->has_lock = 1;
}

static int measure_multiplex(const char *config_path, int adapter,
                             ScanMultiplex *multiplex) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char adapter_arg[16];
        snprintf(adapter_arg, sizeof(adapter_arg), "%d", adapter);
        execlp("dvbv5-zap", "dvbv5-zap",
               "-a", adapter_arg, "-c", config_path, "-x", "-v",
               multiplex->channel_name, NULL);
        _exit(127);
    }

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }

    close(pipefd[1]);
    FILE *output = fdopen(pipefd[0], "r");
    if (output) {
        char line[512];
        while (fgets(line, sizeof(line), output)) {
            parse_signal_sample(multiplex, line);
        }
        fclose(output);
    } else {
        close(pipefd[0]);
    }

    int status;
    waitpid(pid, &status, 0);
    if (multiplex->sample_count > 0) {
        multiplex->signal_dbm /= multiplex->sample_count;
        multiplex->cnr_db /= multiplex->sample_count;
    }
    multiplex->weak = !multiplex->has_lock ||
                      multiplex->cnr_db < MIN_RELIABLE_CNR_DB;
    return multiplex->has_lock;
}

static int comment_weak_sections(const char *config_path,
                                 ScanMultiplex multiplexes[],
                                 const int section_mux[], int section_count) {
    char temp_path[1024];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", config_path) >=
        (int)sizeof(temp_path)) {
        return 0;
    }

    int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) return 0;

    struct stat original_stat;
    if (stat(config_path, &original_stat) == 0) {
        fchmod(temp_fd, original_stat.st_mode & 0777);
    }

    FILE *source = fopen(config_path, "r");
    FILE *dest = fdopen(temp_fd, "w");
    if (!source || !dest) {
        if (source) fclose(source);
        if (dest) fclose(dest); else close(temp_fd);
        unlink(temp_path);
        return 0;
    }

    char line[512];
    int section = -1;
    int disabled = 0;
    while (fgets(line, sizeof(line), source)) {
        char *trimmed = skip_space(line);
        size_t length = strcspn(trimmed, "\r\n");
        if (trimmed[0] == '[' && length > 2 && trimmed[length - 1] == ']') {
            section++;
            disabled = 0;
            if (section < section_count && section_mux[section] >= 0) {
                disabled = multiplexes[section_mux[section]].weak;
                if (disabled) {
                    ScanMultiplex *mux = &multiplexes[section_mux[section]];
                    if (mux->has_lock) {
                        fprintf(dest,
                                "# ZapLink disabled weak multiplex: %.1f dBm, "
                                "%.1f dB C/N (minimum %.1f dB)\n",
                                mux->signal_dbm, mux->cnr_db,
                                MIN_RELIABLE_CNR_DB);
                    } else {
                        fprintf(dest,
                                "# ZapLink disabled weak multiplex: no tuner lock\n");
                    }
                }
            }
        }

        if (disabled) fprintf(dest, "# %s", line);
        else fputs(line, dest);
    }

    int ok = !ferror(source) && fflush(dest) == 0 && fsync(temp_fd) == 0;
    fclose(source);
    if (fclose(dest) != 0) ok = 0;

    if (ok && rename(temp_path, config_path) == 0) return 1;
    unlink(temp_path);
    return 0;
}

int scanner_validate_signals(const char *config_path, int adapter,
                             int include_weak) {
    ScanMultiplex multiplexes[MAX_MULTIPLEXES];
    int section_mux[MAX_SCAN_SECTIONS];
    int mux_count = 0;
    int section_count = 0;

    if (!load_scan_multiplexes(config_path, multiplexes, &mux_count,
                               section_mux, &section_count)) {
        printf("[SCANNER] No multiplexes available for signal validation.\n");
        return -1;
    }

    printf("\n[SCANNER] Validating %d multiplexes (minimum %.1f dB C/N)...\n",
           mux_count, MIN_RELIABLE_CNR_DB);
    int weak_count = 0;
    for (int i = 0; i < mux_count; i++) {
        measure_multiplex(config_path, adapter, &multiplexes[i]);
        if (multiplexes[i].weak) weak_count++;

        double mhz = strtod(multiplexes[i].frequency, NULL) / 1000000.0;
        if (multiplexes[i].has_lock) {
            printf(" [%s] %.0f MHz (%s): %.1f dBm, %.1f dB C/N\n",
                   multiplexes[i].weak ? "WEAK" : "OK", mhz,
                   multiplexes[i].channel_name, multiplexes[i].signal_dbm,
                   multiplexes[i].cnr_db);
        } else {
            printf(" [WEAK] %.0f MHz (%s): no tuner lock\n", mhz,
                   multiplexes[i].channel_name);
        }
    }

    if (weak_count == 0) {
        printf("[SCANNER] All multiplexes meet the signal threshold.\n");
    } else if (include_weak) {
        printf("[SCANNER] Retaining %d weak multiplex%s by user request.\n",
               weak_count, weak_count == 1 ? "" : "es");
    } else if (comment_weak_sections(config_path, multiplexes, section_mux,
                                     section_count)) {
        printf("[SCANNER] Commented out %d weak multiplex%s in %s.\n",
               weak_count, weak_count == 1 ? "" : "es", config_path);
    } else {
        printf("[SCANNER] Warning: failed to comment weak channels in %s.\n",
               config_path);
        return -1;
    }
    return weak_count;
}

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
    int count = (int)g.gl_pathc;
    globfree(&g);
    if (count > MAX_ADAPTERS) {
        printf("[SCANNER] Detected %d adapters; using the first %d.\n",
               count, MAX_ADAPTERS);
        count = MAX_ADAPTERS;
    }
    return count;
}

static int fetch_and_process(const char *zip, const char *scan_file,
                             const char *tmp_html, int skip_vhf) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", RABBIT_EARS_URL, zip);
    
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
    if (!out) return 0;
    
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
    return 1;
}

// Split the master scan list into N temporary files
static int split_scan_list(const char *scan_file, int parts,
                           char part_files[][128]) {
    if (parts < 1 || parts > MAX_ADAPTERS) return 0;
    FILE *in = fopen(scan_file, "r");
    if (!in) return 0;
    
    FILE *outs[MAX_ADAPTERS];
    for (int i = 0; i < parts; i++) {
        snprintf(part_files[i], 128, "%s.part%d", scan_file, i);
        outs[i] = fopen(part_files[i], "w");
        if (!outs[i]) {
            for (int j = 0; j < i; j++) {
                fclose(outs[j]);
                unlink(part_files[j]);
            }
            fclose(in);
            return 0;
        }
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
    return 1;
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

static int config_has_valid_service(const char *path) {
    FILE *input = fopen(path, "r");
    if (!input) return 0;
    char line[512];
    int in_section = 0;
    int has_frequency = 0;
    int has_service = 0;
    int has_vchannel = 0;
    int valid = 0;
    while (fgets(line, sizeof(line), input)) {
        char *text = skip_space(line);
        if (*text == '#' || *text == ';') continue;
        if (*text == '[') {
            if (in_section && has_frequency && has_service && has_vchannel) {
                valid = 1;
                break;
            }
            in_section = 1;
            has_frequency = has_service = has_vchannel = 0;
        } else if (in_section) {
            if (strncmp(text, "FREQUENCY", 9) == 0) has_frequency = 1;
            else if (strncmp(text, "SERVICE_ID", 10) == 0) has_service = 1;
            else if (strncmp(text, "VCHANNEL", 8) == 0) has_vchannel = 1;
        }
    }
    if (!valid && in_section && has_frequency && has_service && has_vchannel) {
        valid = 1;
    }
    fclose(input);
    return valid;
}

static int run_parallel_scan(int num_adapters, char part_files[][128],
                             const char *dest_file) {
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
            
            char out_part[128];
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
        } else {
            close(pipefd[0]);
            close(pipefd[1]);
            LOG_ERROR("SCANNER", "Unable to fork scanner for adapter %d", i);
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
                        workers[i].status_valid =
                            waitpid(workers[i].pid, &workers[i].wait_status, 0) > 0;
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
                         workers[i].wait_status = status;
                         workers[i].status_valid = 1;
                         close(workers[i].pipe_fd);
                         workers[i].pid = 0;
                         active_workers--;
                    }
                 }
            }
        } else if (ret < 0 && errno != EINTR) {
            LOG_ERROR("SCANNER", "select failed: %s", strerror(errno));
            break;
        }
    }

    for (int i = 0; i < num_adapters; i++) {
        if (workers[i].pid > 0) {
            kill(workers[i].pid, SIGTERM);
            workers[i].status_valid =
                waitpid(workers[i].pid, &workers[i].wait_status, 0) > 0;
            close(workers[i].pipe_fd);
            workers[i].pid = 0;
        }
        if (workers[i].status_valid &&
            (!WIFEXITED(workers[i].wait_status) ||
             WEXITSTATUS(workers[i].wait_status) != 0)) {
            LOG_WARN("SCANNER", "Scan worker %d exited unsuccessfully", i);
        }
    }
    
    // Merge
    printf("[SCANNER] Merging results...\n");
    int valid_parts = 0;
    for (int i = 0; i < num_adapters; i++) {
        char part_out[128];
        snprintf(part_out, sizeof(part_out), "%s.out", part_files[i]);
        if (config_has_valid_service(part_out)) valid_parts++;
    }
    if (valid_parts == 0) {
        LOG_ERROR("SCANNER", "No scan worker produced a valid channel service");
        for (int i = 0; i < num_adapters; i++) {
            char part_out[128];
            snprintf(part_out, sizeof(part_out), "%s.out", part_files[i]);
            unlink(part_out);
            unlink(part_files[i]);
        }
        return 0;
    }

    FILE *dest = fopen(dest_file, "w");
    if (!dest) {
        for (int i = 0; i < num_adapters; i++) {
            char part_out[128];
            snprintf(part_out, sizeof(part_out), "%s.out", part_files[i]);
            unlink(part_out);
            unlink(part_files[i]);
        }
        return 0;
    }
    
    for (int i = 0; i < num_adapters; i++) {
        char part_out[128];
        snprintf(part_out, sizeof(part_out), "%s.out", part_files[i]);
        FILE *src = config_has_valid_service(part_out) ? fopen(part_out, "r") : NULL;
        if (src) {
            int ch;
            while ((ch = fgetc(src)) != EOF) fputc(ch, dest);
            fclose(src);
            unlink(part_out); // Cleanup
        }
        unlink(part_files[i]); // Cleanup input part
    }
    int ok = !ferror(dest) && fflush(dest) == 0;
    if (fclose(dest) != 0) ok = 0;
    if (!ok || !config_has_valid_service(dest_file)) {
        unlink(dest_file);
        return 0;
    }
    printf("[SCANNER] Scan complete! Saved to %s\n", dest_file);
    return 1;
}

int scanner_check(const char *config_path, int force_scan) {
    int config_exists = access(config_path, F_OK) == 0;
    if (config_exists && !force_scan) return 0;

    if (!isatty(STDIN_FILENO)) {
        LOG_ERROR("SCANNER",
                  "Interactive channel setup requires a terminal; run zaplink -s manually");
        return -1;
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
    
    printf("%s Run Automatic Channel Scanner? [Y/n]: ",
           config_exists ? "Replace the current channels.conf?" :
                           "No channels.conf found.");
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    if (buf[0] == 'n' || buf[0] == 'N') return config_exists ? 0 : -1;
    
    // Zip Code Input
    char zip[16] = {0};
    while (1) {
        printf("Enter your Zip Code (or leave empty for full scan): ");
        if (!fgets(zip, sizeof(zip), stdin)) return -1;
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
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    if (buf[0] == 'y' || buf[0] == 'Y') skip_vhf = 1;

    // Weak Signal Option
    int include_weak = 0;
    printf("\nInclude weak channels below %.1f dB C/N? Weak reception can\n",
           MIN_RELIABLE_CNR_DB);
    printf("cause Jellyfin streams to fail. Excluded channels remain in\n");
    printf("channels.conf as comments for later review. [y/N]: ");
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    if (buf[0] == 'y' || buf[0] == 'Y') include_weak = 1;
    
    char temp_directory[] = "/tmp/zaplink-scan.XXXXXX";
    if (!mkdtemp(temp_directory)) {
        LOG_ERROR("SCANNER", "Unable to create temporary scan directory: %s",
                  strerror(errno));
        return -1;
    }
    char master_scan_file[64];
    char scan_html[64];
    snprintf(master_scan_file, sizeof(master_scan_file), "%s/master.conf",
             temp_directory);
    snprintf(scan_html, sizeof(scan_html), "%s/rabbitears.html",
             temp_directory);
    if (!fetch_and_process(zip[0] ? zip : "00000", master_scan_file,
                           scan_html, skip_vhf)) {
        LOG_ERROR("SCANNER", "Unable to create the RF scan list");
        unlink(scan_html);
        unlink(master_scan_file);
        rmdir(temp_directory);
        return -1;
    }
    
    printf("\nReady to scan. Please ensure your antenna is connected.\n");
    printf("Press ENTER to start scanning...");
    if (!fgets(buf, sizeof(buf), stdin)) {
        unlink(scan_html);
        unlink(master_scan_file);
        rmdir(temp_directory);
        return -1;
    }
    
    char part_files[MAX_ADAPTERS][128] = {{0}};
    if (!split_scan_list(master_scan_file, adapters, part_files)) {
        LOG_ERROR("SCANNER", "Unable to divide the RF scan list");
        unlink(scan_html);
        unlink(master_scan_file);
        rmdir(temp_directory);
        return -1;
    }
    unlink(master_scan_file);

    char candidate_path[1024];
    if (snprintf(candidate_path, sizeof(candidate_path), "%s.scan.XXXXXX",
                 config_path) >= (int)sizeof(candidate_path)) {
        LOG_ERROR("SCANNER", "Configuration path is too long");
        for (int i = 0; i < adapters; i++) unlink(part_files[i]);
        unlink(scan_html);
        rmdir(temp_directory);
        return -1;
    }
    int candidate_fd = mkstemp(candidate_path);
    if (candidate_fd < 0) {
        LOG_ERROR("SCANNER", "Unable to create scan candidate: %s",
                  strerror(errno));
        for (int i = 0; i < adapters; i++) unlink(part_files[i]);
        unlink(scan_html);
        rmdir(temp_directory);
        return -1;
    }
    close(candidate_fd);

    int scan_ok = run_parallel_scan(adapters, part_files, candidate_path);
    unlink(scan_html);
    rmdir(temp_directory);
    if (!scan_ok) {
        unlink(candidate_path);
        LOG_ERROR("SCANNER", "Channel scan failed; existing configuration was preserved");
        return -1;
    }

    if (scanner_validate_signals(candidate_path, 0, include_weak) < 0) {
        unlink(candidate_path);
        LOG_ERROR("SCANNER", "Signal validation failed; existing configuration was preserved");
        return -1;
    }

    if (!config_has_valid_service(candidate_path)) {
        unlink(candidate_path);
        LOG_ERROR("SCANNER", "Scan candidate contains no active services");
        return -1;
    }

    if (rename(candidate_path, config_path) != 0) {
        LOG_ERROR("SCANNER", "Unable to install scan candidate: %s",
                  strerror(errno));
        unlink(candidate_path);
        return -1;
    }

    printf("\n[SUCCESS] Configuration generated atomically! Exiting.\n");
    return 1;
}

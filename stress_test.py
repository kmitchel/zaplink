
import requests
import time
import subprocess
import shutil
import statistics
import sys
import os

BASE_URL = "http://localhost:18392"
CYAN = "\033[96m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
RESET = "\033[0m"

def log(msg, color=RESET):
    print(f"{color}{msg}{RESET}")

def check_server():
    try:
        requests.get(f"{BASE_URL}/playlist.m3u", timeout=2)
        return True
    except:
        return False

def benchmark_endpoint(name, endpoint, count=50):
    log(f"\nBenchmarking {name} ({count} requests)...", CYAN)
    latencies = []
    
    start_total = time.time()
    for _ in range(count):
        t0 = time.time()
        try:
            r = requests.get(f"{BASE_URL}{endpoint}")
            r.raise_for_status()
            latencies.append((time.time() - t0) * 1000) # ms
        except Exception as e:
            log(f"  Request failed: {e}", RED)
    
    total_time = time.time() - start_total
    
    if not latencies:
        log("  No successful requests.", RED)
        return

    avg = statistics.mean(latencies)
    p95 = statistics.quantiles(latencies, n=20)[18] if len(latencies) >= 20 else max(latencies)
    rps = count / total_time
    
    log(f"  Avg Latency: {avg:.2f} ms", GREEN)
    log(f"  P95 Latency: {p95:.2f} ms", GREEN)
    log(f"  Throughput:  {rps:.2f} req/sec", YELLOW)

def run_transcode_benchmark():
    binary = "./zaplink"
    if not os.path.exists(binary):
        binary = "/opt/zaplink/zaplink"
        
    if not os.path.exists(binary):
        log("  zaplink binary not found (looked in ./ and /opt/zaplink).", RED)
        return

    log("\nRunning Transcode Compatibility Benchmark...", CYAN)
    # Stop service temporarily to free up resources? No, -t acts on standalone ffmpeg calls usually.
    # Zaplink -t doesn't use tuners.
    
    result = subprocess.run([binary, "-t"], capture_output=True, text=True)
    print(result.stdout)

def test_tuner_saturation():
    log("\nTesting Tuner Saturation Rejection...", CYAN)
    # Start streams until failure
    procs = []
    active = 0
    # Provide a list of known good channels (based on logs)
    channels = ["15.1", "55.1", "16.1"] 
    
    for ch in channels:
        log(f"  Requesting stream {ch}...", RESET)
        # Use curl to open stream
        p = subprocess.Popen(["curl", "-s", f"{BASE_URL}/stream/{ch}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(p)
        time.sleep(1) # Wait for tuner lock
        
        # Check if stream is actually running (process still alive)
        if p.poll() is None:
            active += 1
            log(f"  -> Stream {ch} ACTIVE", GREEN)
        else:
            log(f"  -> Stream {ch} FAILED (Expected if tuners full)", YELLOW)
    
    log(f"  Active Streams: {active}", CYAN)
    
    log("  Cleaning up streams...", RESET)
    for p in procs:
        p.terminate()
    try:
        subprocess.run(["killall", "curl"], stderr=subprocess.DEVNULL)
    except: pass

def main():
    log("ZapLink Stress Test Suite", CYAN)
    log("=========================", CYAN)
    
    if not check_server():
        log("Error: ZapLink server is not running on port 18392.", RED)
        sys.exit(1)
        
    benchmark_endpoint("XMLTV Generation (Cached)", "/xmltv.xml", count=100)
    benchmark_endpoint("Playlist Generation (Cached)", "/playlist.m3u", count=100)
    
    if os.path.exists("./zaplink"):
        run_transcode_benchmark()
    else:
        log("\nSkipping internal benchmark (binary not found).", YELLOW)
        
    test_tuner_saturation()

if __name__ == "__main__":
    main()

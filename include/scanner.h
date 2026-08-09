/**
 * @file scanner.h
 * @brief First-run channel scanning wizard
 * 
 * Provides an interactive wizard to scan for available channels
 * when no channels.conf exists. Uses dvbv5-scan to perform
 * frequency scanning and generate the configuration file.
 */

#ifndef SCANNER_H
#define SCANNER_H

/**
 * Check if channel configuration exists and run wizard if needed
 * 
 * If channels.conf is missing:
 * 1. Prompts user to run the setup wizard
 * 2. Guides through adapter selection and frequency scanning
 * 3. Creates channels.conf on success
 * 
 * @param config_path Path to check for channels.conf
 * @param force_scan Run the wizard even if a configuration already exists
 * @return 1 after a successful scan, 0 if no scan was needed, -1 on failure
 */
int scanner_check(const char *config_path, int force_scan);

/**
 * Measure every multiplex in a generated channel configuration.
 * Weak sections are commented out unless include_weak is non-zero.
 *
 * @param config_path Path to the generated channels.conf
 * @param adapter DVB adapter number used for validation
 * @param include_weak Keep weak channel sections active when non-zero
 * @return Number of weak multiplexes found, or -1 on parse failure
 */
int scanner_validate_signals(const char *config_path, int adapter,
                             int include_weak);

#endif

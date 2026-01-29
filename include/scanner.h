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
 * @return 1 if wizard ran and requires restart, 0 to continue normal startup
 */
int scanner_check(const char *config_path);

#endif

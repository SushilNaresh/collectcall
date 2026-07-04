#ifndef CC_ENV_LOADER_H
#define CC_ENV_LOADER_H

/*
 * Load key=value pairs from a file into the process environment.
 * Existing env vars are NOT overridden (real env takes precedence).
 *
 * Returns number of vars loaded, or -1 if the file cannot be opened.
 */
int cc_load_env_file(const char *path);

#endif /* CC_ENV_LOADER_H */

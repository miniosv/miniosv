/*
 * Console geometry, for applications that render to the terminal.
 *
 * ioctl() here returns ENOTTY, so TIOCGWINSZ is not a way to ask. The one
 * thing application code still needs from a terminal is how big it is, so it
 * asks for that directly and for nothing else.
 */

#ifndef OSV_TERMINAL_H
#define OSV_TERMINAL_H

#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills *ws with the console's rows and columns. Returns 0, or -1 with errno
 * set if ws is null. */
int osv_terminal_size(struct winsize *ws);

#ifdef __cplusplus
}
#endif

#endif /* OSV_TERMINAL_H */

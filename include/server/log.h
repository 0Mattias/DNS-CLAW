/*
 * server/log.h — Colored logging utilities
 */
#ifndef CLAW_SERVER_LOG_H
#define CLAW_SERVER_LOG_H

void log_info(const char *tag, const char *fmt, ...);
void log_ok(const char *tag, const char *fmt, ...);
void log_warn(const char *tag, const char *fmt, ...);
void log_err(const char *tag, const char *fmt, ...);

#endif /* CLAW_SERVER_LOG_H */

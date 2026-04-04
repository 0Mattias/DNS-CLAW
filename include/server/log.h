/*
 * server/log.h — Colored logging utilities
 */
#ifndef CLAW_SERVER_LOG_H
#define CLAW_SERVER_LOG_H

void log_info(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_ok(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_warn(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_err(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* CLAW_SERVER_LOG_H */

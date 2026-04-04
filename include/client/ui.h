/*
 * client/ui.h — Banner, help, usage, configuration display
 */
#ifndef CLAW_CLIENT_UI_H
#define CLAW_CLIENT_UI_H

void print_banner(void);
void print_help(void);
void print_usage(const char *argv0);

/* Config subcommand */
void config_show(void);
void config_edit(void);
int config_set(const char *key_value);
int config_provider_interactive(void);

#endif /* CLAW_CLIENT_UI_H */

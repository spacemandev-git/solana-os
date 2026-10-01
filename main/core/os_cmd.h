#pragma once

// Console commands contributed by portable code (Lua host, game services,
// mesh). Both consoles serve them: the badge's UART REPL (core/console.c)
// registers every entry with esp_console at console_start(), and the
// emulator's emu_console() falls back to os_cmd_run(). Register from an init
// function that runs before console_start().

typedef int (*os_cmd_fn_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;       // one line: "lua [list|run <id>|rm <id>]"
    os_cmd_fn_t fn;
} os_cmd_t;

void os_cmd_register(const char *name, const char *help, os_cmd_fn_t fn);
int os_cmd_count(void);
const os_cmd_t *os_cmd_get(int i);
// Runs argv[0] if registered: its return value, or -1 for an unknown command.
int os_cmd_run(int argc, char **argv);

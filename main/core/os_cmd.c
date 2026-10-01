// Portable console command table, see os_cmd.h.

#include <string.h>
#include "esp_log.h"
#include "os_cmd.h"

static const char *TAG = "os_cmd";

#define CMD_MAX 16

static os_cmd_t s_cmds[CMD_MAX];
static int s_n;

void os_cmd_register(const char *name, const char *help, os_cmd_fn_t fn)
{
    if (s_n == CMD_MAX) {
        ESP_LOGE(TAG, "no room for command '%s'", name);
        return;
    }
    s_cmds[s_n++] = (os_cmd_t){ .name = name, .help = help, .fn = fn };
}

int os_cmd_count(void)
{
    return s_n;
}

const os_cmd_t *os_cmd_get(int i)
{
    return i >= 0 && i < s_n ? &s_cmds[i] : NULL;
}

int os_cmd_run(int argc, char **argv)
{
    for (int i = 0; argc > 0 && i < s_n; i++) {
        if (!strcmp(s_cmds[i].name, argv[0])) {
            return s_cmds[i].fn(argc, argv);
        }
    }
    return -1;
}

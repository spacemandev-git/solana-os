// Mesh (ESP-NOW) service, see os_mesh.h and docs/MESH.md.
// STUB: the contract compiles; firmware agent "fw-core" replaces this file
// with the real implementation over os_radio.

#include <string.h>
#include "esp_random.h"
#include "os_mesh.h"

void os_mesh_init(void) {}
bool os_mesh_ready(void) { return false; }
int os_mesh_channel(void) { return 1; }
int os_mesh_peers(os_mesh_peer_t *out, int max) { return 0; }
bool os_mesh_peer(const char *id, os_mesh_peer_t *out) { return false; }
esp_err_t os_mesh_send(os_mesh_type_t type, const char *to_id, const char *json) { return ESP_ERR_NOT_SUPPORTED; }
int os_mesh_subscribe(os_mesh_type_t type, os_mesh_handler_t cb, void *ctx) { return -1; }
void os_mesh_unsubscribe(int handle) {}
bool os_mesh_sign(const char *msg, char out[OS_MESH_TAG_LEN + 1]) { return false; }
void os_mesh_acquire(void) {}
void os_mesh_release(void) {}

void os_mesh_nonce(char out[17])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    for (int i = 0; i < 8; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[16] = 0;
}

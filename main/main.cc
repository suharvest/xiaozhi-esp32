#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_heap_caps.h>

#include "application.h"

#define TAG "main"

// Route all cJSON allocations to PSRAM (fall back to internal RAM if PSRAM is
// exhausted). Large parse trees (e.g. ~8KB per sscma face event frame) would
// otherwise land in internal SRAM and starve it during the wake window when a
// TLS handshake overlaps.
static void* psram_json_malloc(size_t sz) {
    return heap_caps_malloc_prefer(sz, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}
static void psram_json_free(void* p) { free(p); }

extern "C" void app_main(void)
{
    // Install cJSON PSRAM hooks before ANY other initialization — the first
    // cJSON user is sscma_client_init in the Board constructor.
    cJSON_Hooks hooks = { .malloc_fn = psram_json_malloc, .free_fn = psram_json_free };
    cJSON_InitHooks(&hooks);

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}

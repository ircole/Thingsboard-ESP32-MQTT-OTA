 /*
  * @mqttOta.c
 */

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
//#include "esp_event.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "cJSON.h"
#include "mqttOta.h"
#include "wifi.h"

#include "esp_ota_ops.h"
#include "mqtt_client.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_ota_ops.h"
#include "mbedtls/md.h"

/* Chunk size must be a value low enough as to not cause memory shortages
 * but the larger the faster the download.  Note that CHUNK_SIZE is used
 * for the MQTT receive size and for temporarily saving the data, thus
 * the total used will be twice the chunk size.
 */
#define CHUNK_SIZE 30000

/*! Saves bit values used in application */
static EventGroupHandle_t event_group;

/* Saves OTA config received from ThingsBoard*/
struct shared_keys
{
    int fw_size;
    char fw_title[256];
    char fw_checksum[520];
    char fw_checksum_algorithm[32];
    char fw_version[32];
    char target_fw_url[32];
} shared_attributes;

/*! Buffer to save a received MQTT message */
static char mqtt_msg[CHUNK_SIZE + 2];

int chunkCounter = 0;
int numChunks = 0;
int totSize = 0;
char fwTopic[32];
char fwResponse[32];
char cSize[10];
char shaString[65];

const char *rcvdChunk;
int rcvdChunkSize = 0;

char current_version[32];

static esp_mqtt_client_handle_t mqtt_client;
esp_ota_handle_t update_handle = 0;
const esp_partition_t *update_partition = NULL;

unsigned char shaResult[32];

mbedtls_md_context_t ctx;
mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
int calcInit = 0;

static void publishState(char *title, char *version, char *state, char *errorMsg)
{
    ESP_LOGI(TAG, "Publish state: %s", state);
    cJSON *current_fw = cJSON_CreateObject();
    cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_CURRENT_FW_TITLE, title);
    cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_CURRENT_FW, version);
    cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_FW_STATE, state);
    if (errorMsg != NULL)
    {
        cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_FW_ERROR, errorMsg);
    }
    char *current_fw_attribute = cJSON_PrintUnformatted(current_fw);
    cJSON_Delete(current_fw);
    esp_mqtt_client_publish(mqtt_client, TB_TELEMETRY_TOPIC, current_fw_attribute, 0, 1, 0);
    // Free is intentional, it's client responsibility to free the result of cJSON_Print
    free(current_fw_attribute);

}

static void publishCurVer(char *title, char *version)
{
    cJSON *current_fw = cJSON_CreateObject();
    cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_CURRENT_FW_TITLE, title);
    cJSON_AddStringToObject(current_fw, TB_CLIENT_ATTR_FIELD_CURRENT_FW, version);
    char *current_fw_attribute = cJSON_PrintUnformatted(current_fw);
    cJSON_Delete(current_fw);
    esp_mqtt_client_publish(mqtt_client, TB_TELEMETRY_TOPIC, current_fw_attribute, 0, 1, 0);
    // Free is intentional, it's client responsibility to free the result of cJSON_Print
    free(current_fw_attribute);
    vTaskDelay(2000 / portTICK_PERIOD_MS);

}

static void publishFwChunkReq(void)
{
    char cCounter[5];
    sprintf(cSize, "%d", CHUNK_SIZE);
    sprintf(cCounter, "%d", chunkCounter);
    ESP_LOGI(TAG, "Publish Chunk Request.  Chunk size: %d Chunk number: %s", CHUNK_SIZE, cCounter);
    strcpy(fwTopic, TB_FW_REQUEST_TOPIC);
    strcat(fwTopic, cCounter);
    strcpy(fwResponse, TB_FW_RESPONSE_STRING);
    strcat(fwResponse, cCounter);
    esp_mqtt_client_publish(mqtt_client, fwTopic, cSize, 0, 1, 0);
}

static void hexToHexString(unsigned char *input, char *output, int input_len)
{
    int loop = 0;
    int i = 0;
    while (loop < input_len)
    {
        sprintf((char*) (output + i), "%02x", input[loop]);
        loop += 1;
        i += 2;
    }
    output[i++] = '\0';
}

static void addChunk(void)
{
    esp_err_t err;
    int state = STATE_OTA_WRITE;
    if (calcInit == 0)
    {
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
        mbedtls_md_starts(&ctx);
        calcInit = 1;
        memset(shaString, 0, sizeof(shaString));
    }
    totSize += rcvdChunkSize;
    mbedtls_md_update(&ctx, (const unsigned char*) rcvdChunk, rcvdChunkSize);
    while (1)
    {
        switch (state)
        {
        case STATE_OTA_WRITE:
        {
            err = esp_ota_write(update_handle, (const void*) rcvdChunk, rcvdChunkSize);
            if (rcvdChunk != NULL)
            {
                free(rcvdChunk);
                rcvdChunk = NULL;
            }
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "OTA write failed with error: %d ABORTING", err);
                esp_ota_abort(update_handle);
                state = STATE_OTA_ERROR;
            } else
            {
                state = STATE_OTA_REQUEST_NEXT_CHUNK;
            }
            break;
        }
        case STATE_OTA_REQUEST_NEXT_CHUNK:
        {
            if (totSize < shared_attributes.fw_size && chunkCounter < numChunks)
            {
                chunkCounter++;
                publishFwChunkReq();
                state = STATE_EXIT;
            } else
            {
                state = STATE_OTA_DOWNLOADED;
            }
            break;
        }
        case STATE_OTA_DOWNLOADED:
        {
            mbedtls_md_finish(&ctx, shaResult);
            mbedtls_md_free(&ctx);
            ESP_LOGI(TAG, "Download complete. Size received: %d", totSize);
            publishState("UPDATE", current_version, "DOWNLOADED", NULL);
            hexToHexString(shaResult, shaString, sizeof(shaResult));
            chunkCounter = 0;
            calcInit = 0;
            totSize = 0;
            ESP_LOGI(TAG, "SHA256: %s", shaString);
            if (strcasecmp(shaString, shared_attributes.fw_checksum) != 0)
            {
                esp_ota_abort(update_handle);
                ESP_LOGE(TAG, "Checksums don't match, ABORTING.");
                publishState("UPDATE", current_version, "FAILED", "Checksum failed.");
                state = STATE_OTA_ERROR;
            } else
            {
                state = STATE_OTA_END;
            }
            break;
        }
        case STATE_OTA_END:
        {
            publishState("UPDATE", current_version, "VERIFIED", NULL);
            publishState("UPDATE", current_version, "UPDATING", NULL);
            err = esp_ota_end(update_handle);
            if (err != ESP_OK)
            {
                if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                {
                    ESP_LOGE(TAG, "Image validation failed, image is corrupted");
                }
                ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
                publishState("UPDATE", current_version, "FAILED", "Image validation failed, image is corrupted");
                state = STATE_OTA_ERROR;
            } else
            {
                state = STATE_OTA_SET_BOOT;
            }
            break;
        }
        case STATE_OTA_SET_BOOT:
        {
            err = esp_ota_set_boot_partition(update_partition);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                publishState("UPDATE", current_version, "FAILED", "Set Boot partition failed");
                state = STATE_OTA_ERROR;
            } else
            {
                state = STATE_IMAGE_UPDATED;
            }
            break;
        }
        case STATE_IMAGE_UPDATED:
        {
            publishState("UPDATE", current_version, "UPDATING", NULL);
            strcpy(current_version, shared_attributes.fw_version);
            publishState(shared_attributes.fw_title, shared_attributes.fw_version,
            TB_CLIENT_STATE_UPDATED, NULL);
            ESP_LOGI(TAG, "Firmware update success, restarting.");
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            esp_restart();
            break;
        }
        case STATE_OTA_ERROR:
        {
            if (rcvdChunk != NULL)
            {
                free(rcvdChunk);
                rcvdChunk = NULL;
            }
            state = STATE_EXIT;
            break;
        }
        case STATE_EXIT:
        {
        	ESP_LOGI(TAG, "Exiting add chunk.");
            return;
            break;
        }
        }
    }
}

static void clearSharedAttributes(void)
{
    shared_attributes.fw_size = 0;
    memset(shared_attributes.fw_title, 0, sizeof(shared_attributes.fw_title));
    memset(shared_attributes.fw_checksum, 0, sizeof(shared_attributes.fw_checksum));
    memset(shared_attributes.fw_checksum_algorithm, 0, sizeof(shared_attributes.fw_checksum_algorithm));
    memset(shared_attributes.fw_version, 0, sizeof(shared_attributes.fw_version));
}

static int parse_ota_config(const cJSON *object)
{
    int rc = 0;
    clearSharedAttributes();
    if (object != NULL)
    {
        cJSON *target_fw_size = cJSON_GetObjectItem(object,
        TB_SHARED_ATTR_FIELD_FW_SIZE);
        if (cJSON_IsNumber(target_fw_size))
        {
            shared_attributes.fw_size = target_fw_size->valueint;

            numChunks = shared_attributes.fw_size / CHUNK_SIZE;
            ESP_LOGI(TAG, "Received firmware size: %d NumChunks: %d", shared_attributes.fw_size, numChunks);
        }

        cJSON *target_fw_title_response = cJSON_GetObjectItem(object,
        TB_SHARED_ATTR_FIELD_FW_TITLE);
        if (cJSON_IsString(target_fw_title_response) && (target_fw_title_response->valuestring != NULL)
                        && strlen(target_fw_title_response->valuestring) < sizeof(shared_attributes.fw_title))
        {
            memcpy(shared_attributes.fw_title, target_fw_title_response->valuestring, strlen(target_fw_title_response->valuestring));
            shared_attributes.fw_title[sizeof(shared_attributes.fw_title) - 1] = 0;
            ESP_LOGI(TAG, "Received title: %s", shared_attributes.fw_title);
        }

        cJSON *target_fw_checksum_response = cJSON_GetObjectItem(object,
        TB_SHARED_ATTR_FIELD_FW_CHECKSUM);
        if (cJSON_IsString(target_fw_checksum_response) && (target_fw_checksum_response->valuestring != NULL)
                        && strlen(target_fw_checksum_response->valuestring) < sizeof(shared_attributes.fw_checksum))
        {
            memcpy(shared_attributes.fw_checksum, target_fw_checksum_response->valuestring,
                            strlen(target_fw_checksum_response->valuestring));
            shared_attributes.fw_checksum[sizeof(shared_attributes.fw_checksum) - 1] = 0;
            ESP_LOGI(TAG, "Received firmware checksum: %s", shared_attributes.fw_checksum);
        }

        cJSON *target_fw_checksum_algorithm_response = cJSON_GetObjectItem(object,
        TB_SHARED_ATTR_FIELD_FW_CHECKSUM_ALGORITHM);
        if (cJSON_IsString(target_fw_checksum_algorithm_response) && (target_fw_checksum_algorithm_response->valuestring != NULL)
                        && strlen(target_fw_checksum_algorithm_response->valuestring) < sizeof(shared_attributes.fw_checksum_algorithm))
        {
            memcpy(shared_attributes.fw_checksum_algorithm, target_fw_checksum_algorithm_response->valuestring,
                            strlen(target_fw_checksum_algorithm_response->valuestring));
            shared_attributes.fw_checksum_algorithm[sizeof(shared_attributes.fw_checksum_algorithm) - 1] = 0;
            ESP_LOGI(TAG, "Received firmware checksum alogrithm: %s", shared_attributes.fw_checksum_algorithm);
        }

        cJSON *fw_ver_response = cJSON_GetObjectItem(object,
        TB_SHARED_ATTR_FIELD_FW_VER);
        if (cJSON_IsString(fw_ver_response) && (fw_ver_response->valuestring != NULL)
                        && strlen(fw_ver_response->valuestring) < sizeof(shared_attributes.fw_version))
        {
            memcpy(shared_attributes.fw_version, fw_ver_response->valuestring, strlen(fw_ver_response->valuestring));
            shared_attributes.fw_version[sizeof(shared_attributes.fw_version) - 1] = 0;
            ESP_LOGI(TAG, "Received firmware version: %s", shared_attributes.fw_version);
        }
    }
    if (shared_attributes.fw_size == 0)
    {
        rc = -1;
    }
    return rc;
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    assert(event != NULL);
    char ddata[100];
    char dtopic[event->topic_len + 1];
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        xEventGroupClearBits(event_group, MQTT_DISCONNECTED_EVENT);
        xEventGroupSetBits(event_group, MQTT_CONNECTED_EVENT);

        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(mqtt_client,
        TB_ATTRIBUTES_SUBSCRIBE_TO_RESPONSE_TOPIC, 1);
        esp_mqtt_client_subscribe(mqtt_client, TB_ATTRIBUTES_TOPIC, 1);
        esp_mqtt_client_subscribe(mqtt_client, TB_FW_RESPONSE_TOPIC, 1);
        ESP_LOGI(TAG, "Subscribed to shared attributes updates");
    break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(event_group, MQTT_CONNECTED_EVENT);
        xEventGroupSetBits(event_group, MQTT_DISCONNECTED_EVENT);
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
    case MQTT_EVENT_DATA:
        ESP_LOGD (TAG,"topic %s topic_len %d data_len %d total_data_len %d msg_id %d session_present %d data_offset %d\n\n\r", event->topic,
                        event->topic_len, event->data_len, event->total_data_len, event->msg_id, event->session_present,
                        event->current_data_offset);
        memcpy(dtopic, event->topic, event->topic_len);
        dtopic[event->topic_len] = 0;
        ESP_LOGD(TAG, "MQTT_EVENT_DATA, msg_id=%d, %s", event->msg_id, mqtt_msg);
        if (event->data_len >= (sizeof(mqtt_msg) - 1))
        {
            ESP_LOGE(TAG, "Received MQTT message size [%d] more than expected [%d]", event->data_len, (sizeof(mqtt_msg) - 1));
            return ESP_FAIL;
        }

        if (strcmp(TB_ATTRIBUTES_RESPONSE_TOPIC, dtopic) == 0)
        {
            memcpy(mqtt_msg, event->data, event->data_len);
            mqtt_msg[event->data_len] = 0;
            cJSON *attributes = cJSON_Parse(mqtt_msg);
            if (attributes != NULL)
            {
                cJSON *shared = cJSON_GetObjectItem(attributes, "shared");
                parse_ota_config(shared);
            }

            char *attributes_string = cJSON_Print(attributes);
            cJSON_Delete(attributes);
            ESP_LOGI(TAG, "Shared attributes response: %s", attributes_string);
            // Free is intentional, it's client responsibility to free the result of cJSON_Print
            free(attributes_string);

            xEventGroupSetBits(event_group, OTA_CONFIG_FETCHED_EVENT);
        } else if (strcmp(TB_ATTRIBUTES_TOPIC, dtopic) == 0)
        {
            int rc;
            memcpy(mqtt_msg, event->data, MIN(event->data_len, sizeof(mqtt_msg)));
            mqtt_msg[event->data_len] = 0;
            cJSON *attributes = cJSON_Parse(mqtt_msg);
            //cJSON *shared = cJSON_GetObjectItem(attributes, "update");
            char *attributes_string = cJSON_Print(attributes);

            if (strstr(attributes_string, "deleted") == NULL)
            {
                rc = parse_ota_config(attributes);
            } else
            {
                rc = -1;
            }

            cJSON_Delete(attributes);
            ESP_LOGI(TAG, "Shared attributes were updated on ThingsBoard: %s", attributes_string);
            // Free is intentional, it's client responsibility to free the result of cJSON_Print
            free(attributes_string);
            if (rc == 0)
            {
                xEventGroupSetBits(event_group, OTA_CONFIG_UPDATED_EVENT);
            }
        } else if (strcmp(fwResponse, dtopic) == 0)
        {
            rcvdChunk = malloc(event->data_len);
            rcvdChunkSize = event->data_len;
            memcpy(rcvdChunk, event->data, event->data_len);
            xEventGroupSetBits(event_group, MQTT_CHUNK_RECEIVED_EVENT);
        }
    break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    break;
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT");
    break;
    default:
    break;
    }
    return ESP_OK;
}

/**;
 * @brief Main application task, it sends counter value to ThingsBoard telemetry MQTT topic.
 *
 * @param pvParameters Pointer to the task arguments
 */
static void main_application_task(void *pvParameters)
{
    uint8_t counter = 0;

    while (1)
    {
        xEventGroupWaitBits(event_group, OTA_TASK_IN_NORMAL_STATE_EVENT, false, true, portMAX_DELAY);

        counter = counter < 3 ? counter + 1 : 0;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "counter", counter);
        char *post_data = cJSON_PrintUnformatted(root);
        //esp_mqtt_client_publish(mqtt_client, TB_TELEMETRY_TOPIC, post_data, 0, 1, 0);
        cJSON_Delete(root);
        // Free is intentional, it's client responsibility to free the result of cJSON_Print
        free(post_data);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Check is the current running partition label is factory
 *
 * @param running_partition_label Current running partition label
 * @param config_name Configuration name that specified in Kconfig, ThingsBoard OTA configuration submenu
 * @return true If current running partition label is 'factory'
 * @return false If current runnit partition label is not 'factory'
 */
static bool partition_is_factory(const char *running_partition_label, const char *config_name)
{
    if (strcmp(FACTORY_PARTITION_LABEL, running_partition_label) == 0)
    {
        ESP_LOGW(TAG, "Factory partition is running. %s from config is saving to the flash memory", config_name);
        return true;
    } else
    {
        ESP_LOGE(TAG, "%s wasn't found, running partition is not '%s'", config_name, FACTORY_PARTITION_LABEL);
        APP_ABORT_ON_ERROR(ESP_FAIL);
        return false;
    }
}

/**
 * @brief Get MQTT broker URL to use in MQTT client.
 *        If the flash memory is empty and running partition is 'factory'
 *        then MQTT broker URL specified in ThingsBoard OTA configuration submenu will be saved to NVS.
 *        If running partition is not 'factory' ('ota_0' or 'ota_1') then MQTT broker URL from NVS is used.
 *        The application stops if running partition is not 'factory' and MQTT broker URL was not found in NVS.
 *
 * @param running_partition_label Current running partition label
 * @return const char* MQTT broker URL
 */
static const char* get_mqtt_url(const char *running_partition_label)
{
    nvs_handle handle;
    APP_ABORT_ON_ERROR(nvs_open(NVS_KEY_MQTT_URL, NVS_READWRITE, &handle));

    static char mqtt_url[MAX_LENGTH_TB_URL + 1];
    size_t mqtt_url_len = sizeof(mqtt_url);

    esp_err_t result_code = nvs_get_str(handle, NVS_KEY_MQTT_URL, mqtt_url, &mqtt_url_len);
    if (result_code == ESP_OK)
    {
        ESP_LOGI(TAG, "MQTT URL from flash memory: %s", mqtt_url);
    } else if (result_code == ESP_ERR_NVS_NOT_FOUND)
    {
        if (partition_is_factory(running_partition_label, "MQTT URL"))
        {
            APP_ABORT_ON_ERROR(nvs_set_str(handle, NVS_KEY_MQTT_URL, CONFIG_MQTT_BROKER_URL));
            APP_ABORT_ON_ERROR(nvs_commit(handle));
            APP_ABORT_ON_ERROR(nvs_get_str(handle, NVS_KEY_MQTT_URL, mqtt_url, &mqtt_url_len));
        }
    } else
    {
        ESP_LOGE(TAG, "Unable to to get MQTT URL from NVS");
        APP_ABORT_ON_ERROR(ESP_FAIL);
    }

    nvs_close(handle);
    return mqtt_url;
}

/**
 * @brief Get MQTT broker port to use in MQTT client.
 *        If the flash memory is empty and running partition is 'factory'
 *        then MQTT broker port specified in ThingsBoard OTA configuration submenu will be saved to NVS.
 *        If running partition is not 'factory' ('ota_0' or 'ota_1') then MQTT broker port from NVS is used.
 *        The application stops if running partition is not 'factory' and MQTT broker port was not found in NVS.
 *
 * @param running_partition_label Current running partition label
 * @return const char* MQTT broker port
 */
static uint32_t get_mqtt_port(const char *running_partition_label)
{
    nvs_handle handle;
    APP_ABORT_ON_ERROR(nvs_open(NVS_KEY_MQTT_PORT, NVS_READWRITE, &handle));

    uint32_t mqtt_port;

    esp_err_t result_code = nvs_get_u32(handle, NVS_KEY_MQTT_PORT, &mqtt_port);
    if (result_code == ESP_OK)
    {
        ESP_LOGI(TAG, "MQTT port from flash memory: %d", mqtt_port);
    } else if (result_code == ESP_ERR_NVS_NOT_FOUND)
    {
        if (partition_is_factory(running_partition_label, "MQTT port"))
        {
            APP_ABORT_ON_ERROR(nvs_set_u32(handle, NVS_KEY_MQTT_PORT, CONFIG_MQTT_BROKER_PORT));
            APP_ABORT_ON_ERROR(nvs_commit(handle));
            APP_ABORT_ON_ERROR(nvs_get_u32(handle, NVS_KEY_MQTT_PORT, &mqtt_port));
        }
    } else
    {
        ESP_LOGE(TAG, "Unable to to get MQTT port from NVS");
        APP_ABORT_ON_ERROR(ESP_FAIL);
    }

    nvs_close(handle);
    return mqtt_port;
}

/**
 * @brief Get MQTT access token to use in MQTT client.
 *        If the flash memory is empty and running partition is 'factory'
 *        then MQTT broker access token specified in ThingsBoard OTA configuration submenu will be saved to NVS.
 *        If running partition is not 'factory' ('ota_0' or 'ota_1') then MQTT broker access token from NVS is used.
 *        The application stops if running partition is not 'factory' and MQTT broker access token was not found in NVS.
 *
 * @param running_partition_label Current running partition label
 * @return const char* MQTT broker access token
 */
static const char* get_mqtt_access_token(const char *running_partition_label)
{
    nvs_handle handle;
    APP_ABORT_ON_ERROR(nvs_open(NVS_KEY_MQTT_ACCESS_TOKEN, NVS_READWRITE, &handle));

    static char access_token[MAX_LENGTH_TB_ACCESS_TOKEN + 1];
    size_t access_token_len = sizeof(access_token);

    esp_err_t result_code = nvs_get_str(handle, NVS_KEY_MQTT_ACCESS_TOKEN, access_token, &access_token_len);
    if (result_code == ESP_OK)
    {
        ESP_LOGI(TAG, "MQTT access token from flash memory: %s", access_token);
    } else if (result_code == ESP_ERR_NVS_NOT_FOUND)
    {
        if (partition_is_factory(running_partition_label, "MQTT access token"))
        {
            APP_ABORT_ON_ERROR(nvs_set_str(handle, NVS_KEY_MQTT_ACCESS_TOKEN, CONFIG_MQTT_ACCESS_TOKEN));
            APP_ABORT_ON_ERROR(nvs_commit(handle));
            APP_ABORT_ON_ERROR(nvs_get_str(handle, NVS_KEY_MQTT_ACCESS_TOKEN, access_token, &access_token_len));
        }
    } else
    {
        ESP_LOGE(TAG, "Unable to to get MQTT access token from NVS");
        APP_ABORT_ON_ERROR(ESP_FAIL);
    }

    nvs_close(handle);
    return access_token;
}

static void mqtt_app_start(const char *running_partition_label)
{
    assert(running_partition_label != NULL);

    const char *mqtt_url = get_mqtt_url(running_partition_label);
    const uint32_t mqtt_port = get_mqtt_port(running_partition_label);
    const char *mqtt_access_token = get_mqtt_access_token(running_partition_label);

    esp_mqtt_client_config_t mqtt_cfg =
                    { .uri = mqtt_url, .event_handle = mqtt_event_handler, .port = mqtt_port, .buffer_size = CHUNK_SIZE + 100, .username =
                                    mqtt_access_token };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    APP_ABORT_ON_ERROR(esp_mqtt_client_start(mqtt_client));

    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

static bool fw_versions_are_equal(const char *current_ver, const char *target_ver)
{
    assert(current_ver != NULL && target_ver != NULL);
    if (strcasecmp(current_ver, target_ver) == 0)
    {
        ESP_LOGI(TAG, "Skipping OTA, firmware versions are equal - current: %s, target: %s", current_version, shared_attributes.fw_version);
        return true;
    }
    return false;
}

static bool ota_params_are_specified(struct shared_keys ota_config)
{
    if (strlen(ota_config.fw_title) == 0)
    {
        ESP_LOGW(TAG, "Firmware URL is not specified");
        return false;
    }

    if (strlen(ota_config.fw_version) == 0)
    {
        ESP_LOGW(TAG, "Target firmware version is not specified");
        return false;
    }

    return true;
}

static void start_ota(const char *current_ver, struct shared_keys ota_config)
{
    esp_err_t err;
    assert(current_ver != NULL);

    if (strcasecmp(current_ver, shared_attributes.fw_version) != 0)
    {
        ESP_LOGW(TAG, "Starting OTA, firmware versions are different - current: %s, target: %s", current_ver, ota_config.fw_version);
        update_partition = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x", update_partition->subtype, update_partition->address);
        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            //  task_fatal_error();
        }
        else
        {
            publishState("UPDATE", current_version, "DOWNLOADING", NULL);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            publishFwChunkReq();
        }
    }
    else
    {
        ESP_LOGW(TAG, "Starting OTA, Update not required, firmware versions are same - current: %s, target: %s", current_ver, ota_config.fw_version);
    }
}

static enum state connection_state(BaseType_t actual_event, const char *current_state_name)
{
    assert(current_state_name != NULL);

    if (actual_event & WIFI_DISCONNECTED_EVENT)
    {
        ESP_LOGE(TAG, "%s state, Wi-Fi not connected, wait for the connect", current_state_name);
        return STATE_WAIT_WIFI;
    }

    if (actual_event & MQTT_DISCONNECTED_EVENT)
    {
        ESP_LOGW(TAG, "%s state, MQTT not connected, wait for the connect", current_state_name);
        return STATE_WAIT_MQTT;
    }

    return STATE_CONNECTION_IS_OK;
}

/**
 * @brief OTA task, it handles the shared attributes updates and starts OTA if the config received from ThingsBoard is valid.
 *
 * @param pvParameters Pointer to the task arguments
 */
static void ota_task(void *pvParameters)
{
    enum state current_connection_state = STATE_CONNECTION_IS_OK;
    enum state state = STATE_INITIAL;
    BaseType_t ota_events;
    BaseType_t actual_event = 0x00;
    char running_partition_label[sizeof(((esp_partition_t*) 0)->label)];

    while (1)
    {
        if (state != STATE_INITIAL && state != STATE_APP_LOOP)
        {
            if (state != STATE_APP_LOOP)
            {
                xEventGroupClearBits(event_group,
                OTA_TASK_IN_NORMAL_STATE_EVENT);
            }

            actual_event = xEventGroupWaitBits(event_group,
            WIFI_CONNECTED_EVENT | WIFI_DISCONNECTED_EVENT | MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT | OTA_CONFIG_FETCHED_EVENT,
                            false, false, portMAX_DELAY);
        }
        switch (state)
        {
        case STATE_INITIAL:
        {
            // Initialize NVS.
            esp_err_t err = nvs_flash_init();
            if (err == ESP_ERR_NVS_NO_FREE_PAGES)
            {
                // OTA app partition table has a smaller NVS partition size than the non-OTA
                // partition table. This size mismatch may cause NVS initialization to fail.
                // If this happens, we erase NVS partition and initialize NVS again.
                APP_ABORT_ON_ERROR(nvs_flash_erase());
                err = nvs_flash_init();
            }
            APP_ABORT_ON_ERROR(err);

            const esp_partition_t *running_partition = esp_ota_get_running_partition();
            strncpy(running_partition_label, running_partition->label, sizeof(running_partition_label));
            ESP_LOGI(TAG, "Running partition: %s", running_partition_label);

            initialise_wifi(running_partition_label);
            state = STATE_WAIT_WIFI;
            break;
        }
        case STATE_WAIT_WIFI:
        {
            if (actual_event & WIFI_DISCONNECTED_EVENT)
            {
                ESP_LOGW(TAG, "WAIT_WIFI state, Wi-Fi not connected, wait for the connect");
                state = STATE_WAIT_WIFI;
                break;
            }

            if (actual_event & WIFI_CONNECTED_EVENT)
            {
                mqtt_app_start(running_partition_label);
                state = STATE_WAIT_MQTT;
                break;
            }

            ESP_LOGE(TAG, "WAIT_WIFI state, unexpected event received: %d", actual_event);
            state = STATE_INITIAL;
            break;
        }
        case STATE_WAIT_MQTT:
        {
            current_connection_state = connection_state(actual_event, "WAIT_MQTT");
            if (current_connection_state != STATE_CONNECTION_IS_OK)
            {
                state = current_connection_state;
                break;
            }

            if (actual_event & (WIFI_CONNECTED_EVENT | MQTT_CONNECTED_EVENT))
            {
                ESP_LOGI(TAG, "Connected to MQTT broker %s, on port %d", CONFIG_MQTT_BROKER_URL, CONFIG_MQTT_BROKER_PORT);

                // Send the current firmware version to ThingsBoard
                publishCurVer("UPDATE", current_version);
                esp_mqtt_client_publish(mqtt_client,
                TB_ATTRIBUTES_REQUEST_TOPIC,
                TB_SHARED_ATTR_KEYS_REQUEST, 0, 1, 0);
                ESP_LOGI(TAG, "Waiting for shared attributes response");

                state = STATE_WAIT_OTA_CONFIG_FETCHED;
                break;
            }

            ESP_LOGE(TAG, "WAIT_MQTT state, unexpected event received: %d", actual_event);
            state = STATE_INITIAL;
            break;
        }
        case STATE_WAIT_OTA_CONFIG_FETCHED:
        {
            char cSize[10];
            sprintf(cSize, "%d", CHUNK_SIZE);

            current_connection_state = connection_state(actual_event, "WAIT_OTA_CONFIG_FETCHED");
            if (current_connection_state != STATE_CONNECTION_IS_OK)
            {
                state = current_connection_state;
                break;
            }

            if (actual_event & (WIFI_CONNECTED_EVENT | MQTT_CONNECTED_EVENT))
            {
                if (actual_event & OTA_CONFIG_FETCHED_EVENT)
                {
                    ESP_LOGI(TAG, "Shared attributes were fetched from ThingsBoard");
                    xEventGroupClearBits(event_group, OTA_CONFIG_FETCHED_EVENT);
                    state = STATE_OTA_CONFIG_FETCHED;
                    break;
                }
                state = STATE_WAIT_OTA_CONFIG_FETCHED;
                break;
            }

            ESP_LOGE(TAG, "WAIT_OTA_CONFIG_FETCHED state, unexpected event received: %d", actual_event);
            state = STATE_INITIAL;
            break;
        }
        case STATE_OTA_CONFIG_FETCHED:
        {
            current_connection_state = connection_state(actual_event, "OTA_CONFIG_FETCHED");
            if (current_connection_state != STATE_CONNECTION_IS_OK)
            {
                state = current_connection_state;
                break;
            }

            if (actual_event & (WIFI_CONNECTED_EVENT | MQTT_CONNECTED_EVENT))
            {

                start_ota(current_version, shared_attributes);
                state = STATE_APP_LOOP;
                break;
            }
            ESP_LOGE(TAG, "OTA_CONFIG_FETCHED state, unexpected event received: %d", actual_event);
            state = STATE_INITIAL;
            break;
        }
        case STATE_APP_LOOP:
        {
            current_connection_state = connection_state(actual_event, "APP_LOOP");
            if (current_connection_state != STATE_CONNECTION_IS_OK)
            {
                state = current_connection_state;
                break;
            }

            if (actual_event & (WIFI_CONNECTED_EVENT | MQTT_CONNECTED_EVENT))
            {
                ota_events = xEventGroupWaitBits(event_group,
                OTA_CONFIG_UPDATED_EVENT, false, true, 0);
                if ((ota_events & OTA_CONFIG_UPDATED_EVENT))
                {
                    chunkCounter = 0;
                    calcInit = 0;
                    start_ota(current_version, shared_attributes);
                    publishState("UPDATE", current_version, "DOWNLOADING",
                    NULL);
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    publishFwChunkReq();
                }
                ota_events = xEventGroupWaitBits(event_group,
                MQTT_CHUNK_RECEIVED_EVENT, false, true, 0);
                if ((ota_events & MQTT_CHUNK_RECEIVED_EVENT))
                {
                    xEventGroupClearBits(event_group,
                    MQTT_CHUNK_RECEIVED_EVENT);
                    addChunk();
                }
                xEventGroupClearBits(event_group, OTA_CONFIG_UPDATED_EVENT);
                xEventGroupSetBits(event_group, OTA_TASK_IN_NORMAL_STATE_EVENT);
                state = STATE_APP_LOOP;
                break;
            }

            ESP_LOGE(TAG, "APP_LOOP state, unexpected event received: %d", actual_event);
            state = STATE_INITIAL;
            break;
        }
        default:
        {
            ESP_LOGE(TAG, "Unexpected state");
            state = STATE_INITIAL;
            break;
        }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main()
{
    strcpy(current_version, FIRMWARE_VERSION);

    event_group = xEventGroupCreate();
    ESP_LOGI(TAG, "Starting ota_task and main_application_task.");
    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
    xTaskCreate(&main_application_task, "main_application_task", 8192, NULL, 5,
    NULL);
}

void notify_wifi_connected()
{
    xEventGroupClearBits(event_group, WIFI_DISCONNECTED_EVENT);
    xEventGroupSetBits(event_group, WIFI_CONNECTED_EVENT);
}

void notify_wifi_disconnected()
{
    xEventGroupClearBits(event_group, WIFI_CONNECTED_EVENT);
    xEventGroupSetBits(event_group, WIFI_DISCONNECTED_EVENT);
}

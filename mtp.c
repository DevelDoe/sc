#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <ctype.h>
#include <cjson/cJSON.h>

#define PORT 8080

// Function prototype for start_http_server
void start_http_server(); // <-- Add this lin

// Global variable to hold the JSON data (updated via /update endpoint)
static char *global_json_data = NULL;

// Connection-specific structure to accumulate POST data
struct connection_info_struct
{
    char *post_data;
    size_t post_data_size;
};

// Function to remove BOM from JSON data
void remove_bom(char *data)
{
    if (data && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
    {
        memmove(data, data + 3, strlen(data) - 2);
    }
}

// Find symbol in the data set
cJSON *get_symbol_data(cJSON *json_obj, const char *symbol)
{
    size_t n = cJSON_GetArraySize(json_obj);
    for (size_t i = 0; i < n; i++)
    {
        cJSON *obj = cJSON_GetArrayItem(json_obj, i);
        cJSON *symbol_obj = cJSON_GetObjectItem(obj, "symbol");
        if (cJSON_IsString(symbol_obj) && strcmp(symbol_obj->valuestring, symbol) == 0)
        {
            return obj;
        }
    }
    return NULL;
}

// Endpoint to update the JSON stream (expects POST data)
static enum MHD_Result handle_update(void *cls, struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version, const char *upload_data,
                                     size_t *upload_data_size, void **con_cls)
{
    if (*con_cls == NULL)
    {
        struct connection_info_struct *con_info = malloc(sizeof(struct connection_info_struct));
        if (con_info == NULL)
            return MHD_NO;
        con_info->post_data = NULL;
        con_info->post_data_size = 0;
        *con_cls = con_info;
        return MHD_YES;
    }

    struct connection_info_struct *con_info = (struct connection_info_struct *)*con_cls;

    if (strcmp(method, "POST") == 0)
    {
        if (*upload_data_size != 0)
        {
            printf("Receiving POST chunk of size %zu bytes\n", *upload_data_size);
            char *new_data = realloc(con_info->post_data, con_info->post_data_size + *upload_data_size + 1);
            if (new_data == NULL)
                return MHD_NO;
            con_info->post_data = new_data;
            memcpy(con_info->post_data + con_info->post_data_size, upload_data, *upload_data_size);
            con_info->post_data_size += *upload_data_size;
            con_info->post_data[con_info->post_data_size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }
        else
        {
            if (global_json_data)
                free(global_json_data);
            global_json_data = strdup(con_info->post_data ? con_info->post_data : "");

            remove_bom(global_json_data);
            // printf("[DEBUG] Stored JSON after BOM removal: %s\n", global_json_data);

            int object_count = 0;
            cJSON *json_obj = cJSON_Parse(global_json_data);
            if (json_obj && cJSON_IsArray(json_obj))
            {
                object_count = cJSON_GetArraySize(json_obj);
            }
            cJSON_Delete(json_obj);

            // printf("Received update with %d objects.\n", object_count);

            free(con_info->post_data);
            free(con_info);
            *con_cls = NULL;

            char response_buf[256];
            snprintf(response_buf, sizeof(response_buf),
                     "{\"message\": \"Update successful\", \"object_count\": %d}", object_count);
            struct MHD_Response *response = MHD_create_response_from_buffer(strlen(response_buf),
                                                                            response_buf,
                                                                            MHD_RESPMEM_MUST_COPY);
            MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }
    }
    return MHD_NO;
}

// Handler for GET requests on /symbols
static enum MHD_Result handle_symbols(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
    if (!global_json_data)
    {
        const char *error_message = "{\"error\": \"No data available.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                        (void *)error_message,
                                                                        MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    }

    cJSON *parsed_json = cJSON_Parse(global_json_data);
    if (!parsed_json)
    {
        const char *error_message = "{\"error\": \"Failed to parse JSON data.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                        (void *)error_message,
                                                                        MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
    }

    cJSON *symbols_set = cJSON_CreateArray();
    size_t n = cJSON_GetArraySize(parsed_json);
    for (size_t i = 0; i < n; i++)
    {
        cJSON *obj = cJSON_GetArrayItem(parsed_json, i);
        cJSON *symbol_obj = cJSON_GetObjectItem(obj, "symbol");
        if (cJSON_IsString(symbol_obj))
        {
            cJSON_AddItemToArray(symbols_set, cJSON_CreateString(symbol_obj->valuestring));
        }
    }

    cJSON *response_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(response_json, "length", cJSON_GetArraySize(symbols_set));
    cJSON_AddItemToObject(response_json, "symbols", symbols_set);

    char *response_data = cJSON_Print(response_json);
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(response_data),
                                                                    response_data,
                                                                    MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");

    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    cJSON_Delete(parsed_json);
    cJSON_Delete(response_json);
    free(response_data);
    return ret;
}

// Handler for GET requests on /meta/<symbol>
static enum MHD_Result handle_symbol_meta(void *cls, struct MHD_Connection *connection,
                                          const char *url, const char *method,
                                          const char *version, const char *upload_data,
                                          size_t *upload_data_size, void **con_cls)
{
    const char *symbol = url + strlen("/meta/");
    printf("Received GET request for symbol meta: %s\n", symbol);

    if (!global_json_data)
    {
        const char *error_message = "{\"error\": \"No data available.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                        (void *)error_message,
                                                                        MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    }

    cJSON *parsed_json = cJSON_Parse(global_json_data);
    if (!parsed_json)
    {
        const char *error_message = "{\"error\": \"Failed to parse JSON data.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                        (void *)error_message,
                                                                        MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
    }

    cJSON *symbol_obj = get_symbol_data(parsed_json, symbol);
    if (!symbol_obj)
    {
        const char *error_message = "{\"error\": \"Symbol not found.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                        (void *)error_message,
                                                                        MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    }

    char *symbol_data = cJSON_Print(symbol_obj);
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(symbol_data),
                                                                    symbol_data,
                                                                    MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");

    return MHD_queue_response(connection, MHD_HTTP_OK, response);
}

static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version, const char *upload_data,
                                            size_t *upload_data_size, void **con_cls)
{
    printf("Received request: Method=%s, URL=%s\n", method, url);

    // Route POST requests to /update
    if (strncmp(url, "/update", 7) == 0 && strcmp(method, "POST") == 0)
    {
        return handle_update(cls, connection, url, method, version, upload_data, upload_data_size, con_cls);
    }
    // Route GET requests to /meta/<symbol>
    if (strncmp(url, "/meta/", 6) == 0 && strcmp(method, "GET") == 0)
    {
        return handle_symbol_meta(cls, connection, url, method, version, upload_data, upload_data_size, con_cls);
    }
    // Route GET requests to /symbols
    if (strcmp(url, "/symbols") == 0 && strcmp(method, "GET") == 0)
    {
        return handle_symbols(cls, connection, url, method, version, upload_data, upload_data_size, con_cls);
    }
    // Default route for "/"
    if (strcmp(url, "/") == 0)
    {
        const char *response_str = "{\"message\": \"Hello from the API server!\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(response_str),
                                                                        (void *)response_str,
                                                                        MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    printf("Unknown request: Method=%s, URL=%s\n", method, url);
    return MHD_NO;
}

void start_http_server()
{
    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT,
                                                 NULL, NULL,
                                                 &answer_to_connection, NULL,
                                                 MHD_OPTION_END);
    if (daemon == NULL)
    {
        printf("Failed to start HTTP server\n");
        return;
    }
    printf("HTTP API server running on port %d\n", PORT);
    getchar(); // Wait for user input to stop the server
    MHD_stop_daemon(daemon);
}

int main(int argc, char *argv[])
{
    start_http_server();
    return 0;
}

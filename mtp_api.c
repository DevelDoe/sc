#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <json-c/json.h>
#include <ctype.h>

#define PORT 8080

// Global variable to hold the JSON data (updated via /update endpoint)
static char *global_json_data = NULL;

// Connection-specific structure to accumulate POST data
struct connection_info_struct {
    char *post_data;
    size_t post_data_size;
};

// Simple function to remove a UTF-8 BOM from the start of a string.
void remove_bom(char *data) {
    if (!data)
        return;
    if ((unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        size_t len = strlen(data);
        // Shift the entire string left by 3 bytes, including the null terminator.
        memmove(data, data + 3, len - 2);
        printf("BOM removed.\n");
    }
}

// Endpoint to update the JSON stream (expects POST data)
static enum MHD_Result handle_update(void *cls, struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version, const char *upload_data,
                                     size_t *upload_data_size, void **con_cls)
{
    if (*con_cls == NULL) {
        struct connection_info_struct *con_info = malloc(sizeof(struct connection_info_struct));
        if (con_info == NULL)
            return MHD_NO;
        con_info->post_data = NULL;
        con_info->post_data_size = 0;
        *con_cls = con_info;
        return MHD_YES;
    }
    
    struct connection_info_struct *con_info = (struct connection_info_struct *)*con_cls;
    
    if (strcmp(method, "POST") == 0) {
        if (*upload_data_size != 0) {
            // Log each received chunk's size
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
        } else {
            // All POST data received
            if (global_json_data)
                free(global_json_data);
            global_json_data = strdup(con_info->post_data ? con_info->post_data : "");
            // Remove BOM from the JSON data, if present.
            remove_bom(global_json_data);
    
            printf("Finished receiving POST data.\n");
            // Count number of objects in the JSON array
            int object_count = 0;
            struct json_object *json_obj = json_tokener_parse(global_json_data);
            if (json_obj && json_object_get_type(json_obj) == json_type_array) {
                object_count = json_object_array_length(json_obj);
            }
            if (json_obj)
                json_object_put(json_obj);
    
            printf("Received update with %d objects.\n", object_count);
    
            free(con_info->post_data);
            free(con_info);
            *con_cls = NULL;
    
            // Build response JSON with message and object count
            char response_buf[256];
            snprintf(response_buf, sizeof(response_buf),
                     "{\"message\": \"Update successful\", \"object_count\": %d}", object_count);
            printf("Sending response: %s\n", response_buf);
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

// Find symbol in the data set
struct json_object *get_symbol_data(struct json_object *json_obj, const char *symbol) {
    size_t n = json_object_array_length(json_obj);
    for (size_t i = 0; i < n; i++) {
        struct json_object *obj = json_object_array_get_idx(json_obj, i);
        struct json_object *symbol_obj = json_object_object_get(obj, "symbol");
        const char *json_symbol = json_object_get_string(symbol_obj);
        printf("Comparing: '%s' with '%s'\n", json_symbol, symbol);
        if (symbol_obj && strcmp(json_symbol, symbol) == 0) {
            return obj;
        }
    }
    return NULL;
}

// Handler for GET requests on /overview/<symbol>
static enum MHD_Result handle_symbol_overview(void *cls, struct MHD_Connection *connection,
                                              const char *url, const char *method,
                                              const char *version, const char *upload_data,
                                              size_t *upload_data_size, void **con_cls)
{
    // Expect URL pattern: /overview/<symbol>
    const char *symbol = url + strlen("/overview/");
    printf("Received GET request for symbol overview: %s\n", symbol);
    
    if (!global_json_data) {
        const char *error_message = "{\"error\": \"No data available.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                         (void *)error_message,
                                                                         MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        return ret;
    }
    
    // Parse the stored JSON data
    struct json_object *parsed_json = json_tokener_parse(global_json_data);
    if (!parsed_json) {
        const char *error_message = "{\"error\": \"Failed to parse JSON data.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                         (void *)error_message,
                                                                         MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }
    
    // Verify that the data is an array and search for the symbol
    if (json_object_get_type(parsed_json) != json_type_array) {
        const char *error_message = "{\"error\": \"Data format error. Expected JSON array.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                         (void *)error_message,
                                                                         MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        json_object_put(parsed_json);
        return ret;
    }
    
    struct json_object *symbol_obj = get_symbol_data(parsed_json, symbol);
    if (!symbol_obj) {
        printf("Symbol '%s' not found.\n", symbol);
        const char *error_message = "{\"error\": \"Symbol not found.\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(error_message),
                                                                         (void *)error_message,
                                                                         MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        json_object_put(parsed_json);
        return ret;
    }
    
    const char *symbol_data = json_object_to_json_string(symbol_obj);
    printf("Found symbol data: %s\n", symbol_data);
    // Use MUST_COPY so that the response is independent of the internal JSON memory.
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(symbol_data),
                                                                     (void *)symbol_data,
                                                                     MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    json_object_put(parsed_json);
    return ret;
}

// Main router
static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version, const char *upload_data,
                                            size_t *upload_data_size, void **con_cls)
{
    // Route POST requests to /update
    if (strncmp(url, "/update", 7) == 0 && strcmp(method, "POST") == 0) {
        return handle_update(cls, connection, url, method, version, upload_data, upload_data_size, con_cls);
    }
    // Route GET requests to /overview/<symbol>
    if (strncmp(url, "/overview/", 10) == 0 && strcmp(method, "GET") == 0) {
        return handle_symbol_overview(cls, connection, url, method, version, upload_data, upload_data_size, con_cls);
    }
    // Default route for "/"
    if (strcmp(url, "/") == 0) {
        const char *response_str = "{\"message\": \"Hello from the API server!\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(response_str),
                                                                        (void *)response_str,
                                                                        MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json; charset=UTF-8");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }
    return MHD_NO;
}

void start_http_server() {
    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT,
                                                 NULL, NULL,
                                                 &answer_to_connection, NULL,
                                                 MHD_OPTION_END);
    if (daemon == NULL) {
        printf("Failed to start HTTP server\n");
        return;
    }
    printf("HTTP API server running on port %d\n", PORT);
    getchar(); // Wait for user input to stop the server
    MHD_stop_daemon(daemon);
}

int main() {
    start_http_server();
    return 0;
}

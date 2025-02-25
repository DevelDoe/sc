#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// Structure to hold the response data
struct MemoryBuffer
{
    char *data;
    size_t size;
};

// Callback function that writes data to a memory buffer
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (ptr == NULL)
    {
        fprintf(stderr, "Not enough memory (realloc returned NULL)\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0'; // Null-terminate
    return realsize;
}

// Simple function to remove a UTF-8 BOM from the start of a string.
void remove_bom(char *data)
{
    if (!data)
        return;
    if ((unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
    {
        size_t len = strlen(data);
        memmove(data, data + 3, len - 2); // Shift left by 3 bytes (including the null terminator)
        printf("BOM removed.\n");
    }
}

int main()
{
    CURL *curl;
    CURLcode res;

    // Initialize global cURL state.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Loop forever, fetching and posting new data every 3 minutes.
    while (1)
    {
        // Allocate and initialize our memory buffer.
        struct MemoryBuffer chunk;
        chunk.data = malloc(1); // initial allocation; will be grown as needed
        chunk.size = 0;

        // API Endpoint and Query Parameters for Financial Modeling Prep.
        const char *api_key = "RFWWfFzwRZJrDNSxzF4M64RKcuXq3T0O";
        const char *url = "https://financialmodelingprep.com/api/v3/stock-screener";
        const char *market_cap_upper = "2000000000";
        const char *price_lower = "1.75";
        const char *price_upper = "10";
        const char *min_volume = "3000000";
        const char *exchange = "NASDAQ";
        const char *trading = "true";
        const char *etf = "false";
        const char *fund = "false";

        // Build the query URL with parameters.
        char query[1024];
        snprintf(query, sizeof(query),
                 "%s?marketCapLowerThan=%s&priceMoreThan=%s&priceLowerThan=%s&isActivelyTrading=%s&isEtf=%s&isFund=%s&volumeMoreThan=%s&exchange=%s&apikey=%s",
                 url, market_cap_upper, price_lower, price_upper, trading, etf, fund, min_volume, exchange, api_key);

        // Initialize a cURL handle for the GET request.
        curl = curl_easy_init();
        if (curl)
        {
            curl_easy_setopt(curl, CURLOPT_URL, query);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Add this line

            res = curl_easy_perform(curl);
            long get_response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &get_response_code);
            printf("GET response code: %ld\n", get_response_code);

            if (res != CURLE_OK)
            {
                fprintf(stderr, "GET request failed: %s\n", curl_easy_strerror(res));
            }
            else
            {
                printf("GET response received (%lu bytes)\n", (unsigned long)chunk.size);
                struct json_object *parsed_json = json_tokener_parse(chunk.data);
                if (parsed_json)
                {
                    if (json_object_get_type(parsed_json) == json_type_array)
                    {
                        int num_objects = json_object_array_length(parsed_json);
                        printf("Number of objects received: %d\n", num_objects);
                    }
                    json_object_put(parsed_json);
                }
                else
                {
                    fprintf(stderr, "Failed to parse JSON response\n");
                }
            }
            curl_easy_cleanup(curl);
        }

        // Now, POST the resulting JSON to the update endpoint.
        CURL *curl_post = curl_easy_init();
        if (curl_post)
        {
            const char *update_url = "http://localhost:8080/update";
            curl_easy_setopt(curl_post, CURLOPT_URL, update_url);
            curl_easy_setopt(curl_post, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_post, CURLOPT_POSTFIELDS, chunk.data);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Add this line

            // Set HTTP header for JSON content.
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");
            curl_easy_setopt(curl_post, CURLOPT_HTTPHEADER, headers);

            // Allocate a separate memory buffer for the POST response.
            struct MemoryBuffer post_response;
            post_response.data = malloc(1);
            post_response.size = 0;
            curl_easy_setopt(curl_post, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl_post, CURLOPT_WRITEDATA, (void *)&post_response);

            res = curl_easy_perform(curl_post);
            long post_response_code = 0;
            curl_easy_getinfo(curl_post, CURLINFO_RESPONSE_CODE, &post_response_code);
            printf("POST response code: %ld\n", post_response_code);

            if (res != CURLE_OK)
            {
                fprintf(stderr, "POST request failed: %s\n", curl_easy_strerror(res));
            }
            else
            {
                // Parse response JSON from the update endpoint.
                struct json_object *post_json = json_tokener_parse(post_response.data);
                if (post_json)
                {
                    struct json_object *msg_obj, *count_obj;
                    if (json_object_object_get_ex(post_json, "message", &msg_obj))
                    {
                        printf("Server message: %s\n", json_object_get_string(msg_obj));
                    }
                    if (json_object_object_get_ex(post_json, "object_count", &count_obj))
                    {
                        printf("Number of objects sent: %d\n", json_object_get_int(count_obj));
                    }
                    json_object_put(post_json);
                }
                else
                {
                    printf("POST response: %s\n", post_response.data);
                }
            }
            free(post_response.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl_post);
        }

        free(chunk.data);

#ifdef _WIN32
        Sleep(180000); // 180000 milliseconds = 3 minutes
        // Sleep(3000000); // 3000000 milliseconds = 5 minutes
#else
        sleep(180); // 180 seconds
#endif
        printf("Looping again...\n\n");
    }

    // This code is never reached because of the infinite loop.
    curl_global_cleanup();
    return 0;
}

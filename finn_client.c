// Required Headers
// include standard C libraries and libwebsockets.
#include <libwebsockets.h> // WebSocket library
#include <string.h>        // For handling strings
#include <stdio.h>         // For input/output (printf)
#include <time.h>          // Required for timestamps

#include <stdio.h>
#include <string.h>

#define DEBUG_MODE 0   // Set to 0 to turn off debug logs, 1 to enable debug logs
#define MAX_TRADES 300 // Assume ~1 trade per second, so 300 trades = 5 min

#define NUM_SYMBOLS 2 // Define the number of symbols you are tracking
#define MAX_SYMBOLS 50 // Define the maximum number of symbols to track

// const char *symbols[NUM_SYMBOLS] = {"BINANCE:BTCUSDT"};
const char *symbols[NUM_SYMBOLS] = {"BINANCE:BTCUSDT", "BINANCE:ETHUSDT"};
// const char *symbols[NUM_SYMBOLS] = {"BINANCE:BTCUSDT", "BINANCE:ETHUSDT", "BINANCE:XRPUSDT"};

// Define a structure for each symbol's trade data
typedef struct
{
    double price;
} Trade;

// Array of trade data, one for each symbol
Trade trade_data[MAX_SYMBOLS][MAX_TRADES] = {0}; // Initialize trade_data arrays for each symbol
int trade_index[MAX_SYMBOLS] = {0}; // Tracks the latest trade for each symbol

// Function to store the trade for a specific symbol
void store_trade(int symbol_index, double price)
{
    if (DEBUG_MODE)
    {
        printf("[DEBUG] Storing trade for %s: price = %.6f, trade_index = %d\n", symbols[symbol_index], price, trade_index[symbol_index]);
    }

    trade_data[symbol_index][trade_index[symbol_index]].price = price; // Store the price for the specific symbol
    trade_index[symbol_index] = (trade_index[symbol_index] + 1) % MAX_TRADES; // Increment index in a circular buffer fashion

    if (DEBUG_MODE)
    {
        printf("[DEBUG] Stored trade at index %d, Next index: %d\n", trade_index[symbol_index], (trade_index[symbol_index] + 1) % MAX_TRADES);
    }
}

// Function to return the index of a symbol
int get_symbol_index(const char *symbol)
{
    for (int i = 0; i < NUM_SYMBOLS; i++)
    {
        if (strcmp(symbols[i], symbol) == 0)
        {
            return i;
        }
    }
    return -1; // Return -1 if symbol is not found
}


// Function to check for price movement for a specific symbol
void check_price_movement(int symbol_index)
{
    if (DEBUG_MODE)
    {
        printf("[DEBUG] Entering check_price_movement() for %s, trade_index = %d\n", symbols[symbol_index], trade_index[symbol_index]);
    }

    if (trade_index[symbol_index] < 2)
    {
        if (DEBUG_MODE)
        {
            printf("[DEBUG] Not enough data yet for %s (trade_index < 2)\n", symbols[symbol_index]);
        }
        return;
    }

    // Calculate old and new index for the specific symbol
    int old_index = (trade_index[symbol_index] - 2 + MAX_TRADES) % MAX_TRADES; // Refer to the index from 5 minutes ago
    int new_index = (trade_index[symbol_index] - 1 + MAX_TRADES) % MAX_TRADES; // Refer to the latest trade

    if (DEBUG_MODE)
    {
        printf("[DEBUG] old_index = %d, new_index = %d\n", old_index, new_index);
    }

    // Ensure indices are within bounds for the specific symbol
    if (old_index < 0 || old_index >= MAX_TRADES || new_index < 0 || new_index >= MAX_TRADES)
    {
        printf("[ERROR] Invalid indices! old_index = %d, new_index = %d\n", old_index, new_index);
        return;
    }

    double old_price = trade_data[symbol_index][old_index].price;
    double new_price = trade_data[symbol_index][new_index].price;

    if (DEBUG_MODE)
    {
        printf("[DEBUG] trade_data[old_index].price = %.6f, trade_data[new_index].price = %.6f\n", old_price, new_price);
    }

    // Avoid division by zero and ensure old_price is valid
    if (old_price <= 0)
    {
        if (DEBUG_MODE)
        {
            printf("[DEBUG] old_price is invalid (<= 0)! Skipping price movement check for %s.\n", symbols[symbol_index]);
        }
        return;
    }

    // Calculate percentage change
    double change = ((new_price - old_price) / old_price) * 100.0;

    if (DEBUG_MODE)
    {
        printf("[DEBUG] Calculated change: %.6f%%\n", change);
    }

    // Apply a sanity check to filter out large, unrealistic changes
    if (change > 50.0 || change < -50.0)
    {
        if (DEBUG_MODE)
        {
            printf("[DEBUG] Change too large for %s, ignoring this price movement.\n", symbols[symbol_index]);
        }
        return;
    }

    // If price moved by more than 0.01%, trigger alert
    if (change >= 0.01 || change <= -0.01)
    {
        printf("ALERT: %s price moved %.6f%% in the last 5 minutes!\n", symbols[symbol_index], change);
    }
}

// The WebSocket Callback
// This function is called automatically by libwebsockets when something happens on the WebSocket.
// The reason parameter tells us what event occurred.
// The WebSocket Callback
// The WebSocket Callback
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("Connected to WebSocket!\n");

        // Send subscription messages for multiple symbols
        for (int i = 0; i < NUM_SYMBOLS; i++)
        {
            char subscribe_msg[256];
            snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symbols[i]);
            unsigned char buf[LWS_PRE + 256];
            unsigned char *msg = &buf[LWS_PRE];
            memcpy(msg, subscribe_msg, strlen(subscribe_msg));
            lws_write(wsi, msg, strlen(subscribe_msg), LWS_WRITE_TEXT);
            printf("Sent subscription message: %s\n", subscribe_msg);
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        {
            // Extract the symbol dynamically from the received message
            char *symbol_ptr = strstr((char *)in, "\"s\":\"");
            if (!symbol_ptr)
            {
                if (DEBUG_MODE)
                    printf("[DEBUG] No symbol found in message: %.*s\n", (int)len, (char *)in);
                break;  // Skip this message if no symbol is found
            }
            symbol_ptr += 5; // Move pointer after "\"s\":\""
            char symbol[32];
            int i = 0;
            while (symbol_ptr[i] != '"' && symbol_ptr[i] != '\0') // Extract symbol until next quote
            {
                symbol[i] = symbol_ptr[i];
                i++;
            }
            symbol[i] = '\0'; // Null-terminate the symbol string

            int symbol_index = get_symbol_index(symbol); // Find the index of the symbol
            if (symbol_index == -1)
            {
                if (DEBUG_MODE)
                    printf("[ERROR] Unknown symbol: %s\n", symbol);
                break; // Skip processing if the symbol is unknown
            }

            // Extract the price
            char *price_ptr = strstr((char *)in, "\"p\":");
            if (!price_ptr)
            {
                if (DEBUG_MODE)
                    printf("[DEBUG] No price found in message: %.*s\n", (int)len, (char *)in);
                break; // Skip if no price found
            }

            double price = atof(price_ptr + 4); // Extract the price value
            store_trade(symbol_index, price);    // Store the price for the symbol
            check_price_movement(symbol_index);  // Check for significant price movement
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf("Connection error!\n");
        break;

    case LWS_CALLBACK_CLOSED:
        printf("WebSocket closed.\n");
        break;

    default:
        break;
    }
    return 0;
}

int main()
{

    // WEBSOCKET CONFIGURATION
    struct lws_context_creation_info info = {0}; // → This struct holds configuration details for WebSockets.

    // Defines which protocol we are using
    struct lws_protocols protocols[] = {
        {"ws-protocol", callback_ws, 0, 0}, // → callback_ws is the custom name for our WebSocket that we have defined earlier.
        {NULL, NULL, 0, 0}};

    info.port = CONTEXT_PORT_NO_LISTEN;                  // → Since this is a client, we don’t need a port to accept connections.
    info.protocols = protocols;                          // → Assign protocols
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT; // 🔹 Enable SSL/TLS

    struct lws_context *context = lws_create_context(&info); // → Creates the WebSocket context using our configuration.
    if (!context)                                            // → If this fails, we print "Failed to create WebSocket context!" and exit.
    {
        printf("Failed to create WebSocket context!\n");
        return 1;
    }

    // CONNECT FINNHUB
    struct lws_client_connect_info conn_info = {0}; // → Connection config, Holds connection details.

    conn_info.context = context;                                         // → Uses the WebSocket context we created
    conn_info.address = "ws.finnhub.io";                                 // → Finnhub WebSocket URL
    conn_info.port = 443;                                                // → WebSockets use port 443 (SSL)
    conn_info.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0"; // → API Key for authentication
    conn_info.host = conn_info.address;
    conn_info.origin = conn_info.address;
    conn_info.protocol = "ws-protocol";        // → Uses the protocol we defined earlier
    conn_info.ssl_connection = LCCSCF_USE_SSL; // → Enable SSL/TLS

    struct lws *wsi = lws_client_connect_via_info(&conn_info);
    if (!wsi)
    {
        printf("Failed to connect to WebSocket!\n");
        lws_context_destroy(context);
        return 1;
    }

    // Once connected, we need to keep listening for messages.
    // → Keeps running indefinitely, waiting for WebSocket messages.
    while (1)
    {
        lws_service(context, 0); // → This function checks for incoming data and handles events.
    }

    lws_context_destroy(context); // → Cleans up the WebSocket when the program exits.
    return 0;
}
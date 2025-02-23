#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PORT 9090

static int callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    switch (reason)
    {
    case LWS_CALLBACK_RECEIVE:
        printf("Received message: %s\n", (char *)in);
        lws_write(wsi, (unsigned char *)in, len, LWS_WRITE_TEXT); // Echo message back
        break;
    case LWS_CALLBACK_ESTABLISHED:
        printf("New connection established\n");
        break;
    case LWS_CALLBACK_CLOSED:
        printf("Connection closed\n");
        break;
    default:
        break;
    }
    return 0;
}

// WebSocket protocol definition
static struct lws_protocols protocols[] = {
    {
        "http-only", // protocol name
        callback,    // callback function
        0,           // per session data size
        0,           // max message size
    },
    {NULL, NULL, 0, 0} // NULL marks end of protocol list
};

// WebSocket context creation and server setup
int main()
{
    struct lws_context_creation_info info;
    struct lws_context *context;
    memset(&info, 0, sizeof(info));

    info.port = PORT;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    context = lws_create_context(&info);

    if (!context)
    {
        printf("WebSocket server failed to start\n");
        return 1;
    }

    printf("WebSocket server running on port %d\n", PORT);

    while (1)
    {
        lws_service(context, 0); // Main loop to service incoming connections
    }

    lws_context_destroy(context);
    return 0;
}
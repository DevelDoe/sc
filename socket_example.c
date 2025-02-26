#include <arpa/inet.h>   // sockaddr_in, htons(), etc.
#include <stdio.h>       // standard input/output (printf, perror)
#include <stdlib.h>      // standard library (exit)
#include <string.h>      // string handling functions (strlen)
#include <sys/socket.h>  // socket(), bind(), listen(), accept(), etc.
#include <unistd.h>      // close() function

#define PORT 8080

int main() {
    // Declare an integer variable named server_fd (usually means "server file descriptor") which stores the reference to your socket.
    int server_fd = new_socket;

    // This line defines a structure of type sockaddr_in named address.
    // struct sockaddr_in:
    //     * A structure from the <netinet/in.h> header, specifically used to handle Internet addresses for IPv4.
    // Here's the structure simplified:
    //     struct sockaddr_in {
    //         short sin_family;         // Address family (e.g., AF_INET for IPv4)
    //         unsigned short sin_port;  // Port number, using network byte order (big endian)
    //         struct in_addr sin_addr;  // IP address
    //         char sin_zero[8];         // Padding, usually set to zeros
    //     };
    // You typically use this structure to set up an address to bind your socket to, or to connect to a remote socket.
    //    address.sin_family = AF_INET;
    //    address.sin_addr.s_addr = INADDR_ANY;  // Accept connections from any IP address
    //    address.sin_port = htons(8080);        // Convert port number to network byte order
    //    struct sockaddr_in address;
    struct sockaddr_in address;

    // Calculates the size, in bytes, of the address structure (struct sockaddr_in).
    // This size is required by certain socket functions, like accept().
    // unctions like accept() need to know how large the address structure is.
    // The length is passed as a pointer because the function might update it with the actual size used.
    // Example:
    //     new_socket = accept(server_fd, (struct sockaddr *)&address, &addr_len);
    // addr_len is passed as a pointer: (socklen_t*)&addr_len.
    // he function accept() might update addr_len to the actual size of the address returned.
    // Why socklen_t?
    // In standard socket programming, the type for the address length should technically be socklen_t (rather than int):
    //
    socklen_t addr_len = sizeof(address)

        // This declares and initializes a character array called buffer:
        // Creates a character array of size 1024. Often used as temporary storage for data sent or received through sockets.
        // = {0}: Initializes all elements of the buffer to zero (null characters). This clears the buffer before you start using it, preventing random data from causing unexpected behavior.
        // When receiving data from a client using the socket, you'd typically do something like:
        // int bytes_read = read(new_socket, buffer, 1024);
        // printf("Received data: %s\n", buffer);
        char buffer[1024] = {0};

    // Creates a new TCP/IP socket. (IPv4, TCP socket, default protocol)
    // socket(AF_INET, SOCK_STREAM, 0): Creates a new socket.
    //     * AF_INET: Specifies the address family (IPv4).
    //     * SOCK_STREAM: Indicates it's a TCP socket.
    //     * 0: Uses default protocol for TCP/IP.
    // The function returns an integer called a file descriptor. If successful, the file descriptor (server_fd) will be non-negative. If it fails, it returns -1.
    // Checks if socket creation fails (socket() returns -1 on failure, not 0
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set address and port to listen on
    // Configures the server to listen on IPv4 (AF_INET) addresses.
    // INADDR_ANY: The socket listens on all available interfaces (any IP address).
    // htons(PORT): Converts the host byte order (your computer) to network byte order (big-endian).
    address.sin_family = AF_INET;          // IPv4
    address.sin_addr.s_addr = INADDR_ANY;  // Accept any incoming IP address
    address.sin_port = htons(PORT);        // Host-to-Network byte order conversion

    // Bind Socket to Address and Port
    // Associates your socket (server_fd) with the specified address (address) and port (8080).
    // If binding fails, it displays an error and exits.
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for Incoming Connections
    if (listen(server_fd, 3) < 0) {
    }
}
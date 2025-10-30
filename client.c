/*
 * ========================================
 * client.c
 * =Description: The client for the
 * Banking Management System.
 * - Connects to the server
 * - Parses the server's [STATUS]:[Message] protocol
 * - Handles regular and masked input
 *
 * =Compile command:
 * gcc client.c -o client
 * ========================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define BUFFER_SIZE 4096

// --- Function Prototypes ---
void main_communication_loop(int server_fd);
void handle_server_response(char* line, int server_fd);
int parse_server_response(char* response, char* status_out, char* message_out, int buf_size);
void print_message(const char* msg);
void get_user_input(char* buffer, int size);
void get_masked_input(char* buffer, int size);
void client_sigint_handler(int signum);

// --- Global for Graceful Shutdown ---
static volatile int g_client_fd = -1;

int main() {
    int server_fd;
    struct sockaddr_in server_addr;

    signal(SIGINT, client_sigint_handler);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("Client socket created.\n");
    
    g_client_fd = server_fd;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection to server failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Connected to server at %s:%d\n", SERVER_IP, SERVER_PORT);

    main_communication_loop(server_fd);

    // --- Cleanup ---
    if (g_client_fd != -1) {
        close(g_client_fd);
    }
    printf("Connection closed.\n");
    return 0;
}

/**
 * @brief The main loop for handling server communication.
 * Reads data and processes it message by message.
 */
void main_communication_loop(int server_fd) {
    char server_buffer[BUFFER_SIZE];
    int bytes_read;
    char* line;
    char* saveptr = NULL;

    // Read in a loop
    while ((bytes_read = read(server_fd, server_buffer, sizeof(server_buffer) - 1)) > 0) {
        server_buffer[bytes_read] = '\0'; // Null-terminate the read data

        // Process the buffer, which may contain multiple \n terminated messages
        line = strtok_r(server_buffer, "\n", &saveptr);
        
        while (line != NULL) {
            handle_server_response(line, server_fd);
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("\nServer closed the connection.\n");
        } else {
            perror("\nRead from server failed");
        }
    }
}

/**
 * @brief Handles a single, newline-terminated message from the server.
 */
void handle_server_response(char* line, int server_fd) {
    char status[64];
    char message[BUFFER_SIZE];
    char user_buffer[BUFFER_SIZE];

    if (parse_server_response(line, status, message, sizeof(message))) {
        if (strcmp(status, "PROMPT") == 0) {
            print_message(message);
            get_user_input(user_buffer, sizeof(user_buffer));
            write(server_fd, user_buffer, strlen(user_buffer));

        } else if (strcmp(status, "PROMPT_MASKED") == 0) {
            print_message(message);
            get_masked_input(user_buffer, sizeof(user_buffer));
            write(server_fd, user_buffer, strlen(user_buffer));

        } else if (strcmp(status, "SUCCESS") == 0) {
            print_message(message);
            printf("\n");

        } else if (strcmp(status, "ERROR") == 0) {
            printf("\n[SERVER ERROR]: ");
            print_message(message);
            printf("\n");

        } else if (strcmp(status, "LOGOUT") == 0) {
            print_message(message);
            printf("\n");
            close(server_fd);
            g_client_fd = -1;
            exit(0); // Exit the client program

        } else {
            // Default: just print whatever we got
            print_message(line);
            printf("\n");
        }
    } else {
        // Malformed, but we print it anyway for debugging
        printf("Malformed response: %s\n", line);
    }
}

/**
 * @brief Parses the "STATUS:Message" protocol.
 * @return 1 on success, 0 on failure (no colon found).
 */
int parse_server_response(char* response, char* status_out, char* message_out, int buf_size) {
    char* colon = strchr(response, ':');
    if (colon == NULL) {
        return 0; // No colon, malformed
    }

    int status_len = colon - response;
    strncpy(status_out, response, status_len);
    status_out[status_len] = '\0';

    strncpy(message_out, colon + 1, buf_size - 1);
    message_out[buf_size - 1] = '\0';
    
    return 1;
}

/**
 * @brief Prints a message, correctly interpreting \\n as newlines.
 */
void print_message(const char* msg) {
    for (int i = 0; msg[i] != '\0'; i++) {
        if (msg[i] == '\\' && msg[i+1] == 'n') {
            printf("\n");
            i++; // Skip the 'n'
        } else {
            putchar(msg[i]);
        }
    }
    fflush(stdout); // Ensure it prints immediately
}

/**
 * @brief Gets a line of input from the user (for regular text).
 */
void get_user_input(char* buffer, int size) {
    bzero(buffer, size);
    fgets(buffer, size, stdin);
}

/**
 * @brief Gets masked input from the user by disabling terminal echo.
 */
void get_masked_input(char *buffer, int size) {
    struct termios old_term, new_term;
    int i = 0;
    char ch;

    // Get current terminal settings
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;

    // Disable echo (ECHO) and canonical mode (ICANON)
    new_term.c_lflag &= ~(ECHO | ICANON);

    // Apply new settings
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);

    // Read character by character
    while (i < size - 2) {
        ch = getchar();
        if (ch == '\n') { // Stop on Enter
            break;
        }
        buffer[i++] = ch;
    }
    buffer[i++] = '\n';
    buffer[i] = '\0';

    // Restore original settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\n"); // Move to the next line
}

/**
 * @brief Signal handler for SIGINT (Ctrl+C) on the client.
 */
void client_sigint_handler(int signum) {
    printf("\nCtrl+C received. Closing connection...\n");
    if (g_client_fd != -1) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    exit(0); // Exit the client
}

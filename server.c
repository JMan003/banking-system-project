/*
 * ========================================
 * server.c
 * =Description: The main server file for the
 * Banking Management System.
 * - Listens for connections
 * - Forks a child process for each client
 * - Routes clients to the correct logic handler
 *
 * =Compile command:
 * gcc server.c server_logic.c utils.c -o server -pthread
 * ========================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>

#include "server_logic.h"
#include "utils.h"

#define SERVER_PORT 8080

// --- Global Buffers ---
char g_read_buffer[1024];
char g_write_buffer[1024];

// --- Function Prototypes ---
void handle_client_connection(int client_socket);
void sigint_handler(int signum);
void sigchld_handler(int signum);

// --- Globals for Graceful Shutdown ---
static volatile sig_atomic_t g_server_running = 1;
static volatile int g_server_fd = -1;

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;

    signal(SIGINT, sigint_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGPIPE, SIG_IGN); 

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    g_server_fd = server_fd;

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) == -1) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    // --- Accept Loop ---
    while (g_server_running) {
        client_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd == -1) {
            // If the handler closed the socket, g_server_running will be 0
            if (g_server_running == 0) {
                break; // Exit loop gracefully
            }
            perror("Accept failed");
            continue;
        }

        // --- Fork for Client ---
        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            close(client_fd);
        } else if (pid == 0) {
            // --- Child Process ---
            close(server_fd); // Child doesn't need the listener
            
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            printf("Connection accepted from %s. Child PID: %d\n", client_ip, getpid());

            handle_client_connection(client_fd);

            printf("Client %s disconnected. Child %d exiting.\n", client_ip, getpid());
            close(client_fd);
            exit(0);
        } else {
            // --- Parent Process ---
            close(client_fd); // Parent doesn't need the client socket
        }
    }

    // --- Shutdown ---
    printf("\nServer shutdown complete.\n");
    return 0;
}

/**
 * @brief Handles the main menu and routing for a connected client.
 */
void handle_client_connection(int client_socket) {
    int choice = 0;

    while (choice != 5) {
        const char* menu =
            "===== Welcome to the Bank =====\\n"
            "1. Customer Login\\n"
            "2. Employee Login\\n"
            "3. Manager Login\\n"
            "4. Admin Login\\n"
            "5. Exit\\n"
            "Enter your choice: ";

        if (send_response(client_socket, "PROMPT", menu) <= 0) {
            perror("write to client failed");
            break;
        }

        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) {
            printf("Client disconnected from main menu.\n");
            break;
        }

        choice = atoi(g_read_buffer);

        switch (choice) {
            case 1: handle_customer_session(client_socket); break;
            case 2: handle_staff_session(client_socket); break;
            case 3: handle_manager_session(client_socket); break;
            case 4: handle_admin_session(client_socket); break;
            case 5:
                printf("Client selected exit from main menu.\n");
                send_response(client_socket, "LOGOUT", "Goodbye.");
                break;
            default:
                send_response(client_socket, "ERROR", "Invalid choice. Please try again.");
                break;
        }
    }
}

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 */
void sigint_handler(int signum) {
    printf("\nSIGINT received. Shutting down server...\n");
    g_server_running = 0;
    if (g_server_fd != -1) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

/**
 * @brief Signal handler for SIGCHLD.
 * Reaps terminated child processes to prevent zombies.
 */
void sigchld_handler(int signum) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

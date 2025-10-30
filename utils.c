/*
 * ========================================
 * utils.c
 * =Description: Implementation of utility functions.
 * ========================================
 */

#include "utils.h"
#include "bank_storage.h"
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

/**
 * @brief Sends a formatted response to the client.
 * Protocol: [STATUS_CODE]:[Message]\n
 */
int send_response(int socket_fd, const char* status, const char* message) {
    char temp_buffer[1024];
    snprintf(temp_buffer, sizeof(temp_buffer), "%s:%s\n", status, message);
    
    return write(socket_fd, temp_buffer, strlen(temp_buffer));
}

/**
 * @brief Reads a single newline-terminated line from a socket.
 */
int read_line(int socket_fd, char* buffer, int max_len) {
    bzero(buffer, max_len);
    int total_bytes = 0;
    char ch;

    while (total_bytes < max_len - 1) {
        int bytes_read = read(socket_fd, &ch, 1);
        if (bytes_read <= 0) {
            return bytes_read;
        }
        if (ch == '\n') {
            break;
        }
        buffer[total_bytes++] = ch;
    }
    buffer[total_bytes] = '\0'; // Null-terminate
    return total_bytes;
}

// --- Session Management Implementation ---

static volatile int g_session_id = -1;
static sem_t* g_session_sem = NULL;

/**
 * @brief Creates or opens a named POSIX semaphore for session locking.
 */
sem_t* create_session_lock(int session_id, char* sem_name_buffer, int buffer_size) {
    snprintf(sem_name_buffer, buffer_size, "/bms_sem_%d", session_id);
    
    sem_t* sem = sem_open(sem_name_buffer, O_CREAT, 0644, 1);
    if (sem == SEM_FAILED) {
        perror("sem_open failed");
        return NULL;
    }
    
    g_session_id = session_id;
    g_session_sem = sem;
    
    return sem;
}

/**
 * @brief Signal handler for SIGINT / SIGTERM to clean up the semaphore.
 */
void handle_unexpected_disconnect(int signum) {
    if (g_session_id != -1 && g_session_sem != NULL) {
        printf("Signal %d caught. Cleaning up lock for session %d.\n", signum, g_session_id);
        sem_post(g_session_sem);
        sem_close(g_session_sem);
        
        char sem_name[50];
        snprintf(sem_name, sizeof(sem_name), "/bms_sem_%d", g_session_id);
        sem_unlink(sem_name);
    }
    exit(1);
}

/**
 * @brief Releases the session lock without sending a logout message.
 * Used for failed logins.
 */
void release_session_lock(int session_id, sem_t* session_sem) {
    if (session_id == -1 || session_sem == NULL) return;

    printf("Session %d lock released.\n", session_id);
    sem_post(session_sem);
    sem_close(session_sem);
    
    char sem_name[50];
    snprintf(sem_name, sizeof(sem_name), "/bms_sem_%d", session_id);
    sem_unlink(sem_name);

    g_session_id = -1;
    g_session_sem = NULL;
}

/**
 * @brief Gracefully ends a user session by releasing the lock AND sending logout.
 * Used for actual user logouts.
 */
void handle_session_logout(int socket_fd, int session_id, sem_t* session_sem) {
    release_session_lock(session_id, session_sem);
    send_response(socket_fd, "LOGOUT", "Logged out successfully.");
}

// --- Database & Logging Implementation ---

/**
 * @brief Finds the byte offset of a CustomerAccount record by its ID.
 */
off_t find_customer_record_offset(int db_fd, int account_id) {
    struct CustomerAccount temp_account;
    lseek(db_fd, 0, SEEK_SET);
    
    off_t current_pos = 0;
    while (read(db_fd, &temp_account, sizeof(temp_account)) == sizeof(temp_account)) {
        if (temp_account.account_id == account_id) {
            return current_pos;
        }
        current_pos = lseek(db_fd, 0, SEEK_CUR);
    }
    return -1; // Not found
}

/**
 * @brief Finds the byte offset of an EmployeeRecord record by its ID.
 */
off_t find_staff_record_offset(int db_fd, int employee_id) {
    struct EmployeeRecord temp_staff;
    lseek(db_fd, 0, SEEK_SET);
    
    off_t current_pos = 0;
    while (read(db_fd, &temp_staff, sizeof(temp_staff)) == sizeof(temp_staff)) {
        if (temp_staff.employee_id == employee_id) {
            return current_pos;
        }
        current_pos = lseek(db_fd, 0, SEEK_CUR);
    }
    return -1; // Not found
}

/**
 * @brief Finds the byte offset of a LoanApplication record by its ID.
 */
off_t find_loan_record_offset(int db_fd, int loan_id) {
    struct LoanApplication temp_loan;
    lseek(db_fd, 0, SEEK_SET);
    
    off_t current_pos = 0;
    while (read(db_fd, &temp_loan, sizeof(temp_loan)) == sizeof(temp_loan)) {
        if (temp_loan.loan_id == loan_id) {
            return current_pos;
        }
        current_pos = lseek(db_fd, 0, SEEK_CUR);
    }
    return -1; // Not found
}


/**
 * @brief Appends a transaction record to the transaction database.
 */
void log_transaction(int account_id, const char* type, double amount, double new_balance) {
    struct Transaction log_entry;
    log_entry.account_id = account_id;
    log_entry.resulting_balance = new_balance;

    time_t now = time(NULL);
    strftime(log_entry.timestamp, sizeof(log_entry.timestamp), "%Y-m-d %H:%M:%S", localtime(&now));
    snprintf(log_entry.description, sizeof(log_entry.description), "%s: %+.2f", type, amount);

    int log_fd = open(TRANSACTION_DB_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd == -1) {
        perror("CRITICAL: Failed to open transaction log");
        return;
    }

    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(log_fd, F_SETLKW, &lock);
    write(log_fd, &log_entry, sizeof(log_entry));
    lock.l_type = F_UNLCK;
    fcntl(log_fd, F_SETLK, &lock);
    close(log_fd);
}

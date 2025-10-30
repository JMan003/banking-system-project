/*
 * ========================================
 * utils.h
 * =Description: Prototypes for utility functions
 * used by the server, such as socket I/O,
 * session management, and logging.
 * ========================================
 */

#ifndef UTILS_H
#define UTILS_H

#include <semaphore.h>
#include <sys/types.h>  // For off_t

// --- Socket Communication ---
int send_response(int socket_fd, const char* status, const char* message);
int read_line(int socket_fd, char* buffer, int max_len);

// --- Session Management ---
sem_t* create_session_lock(int session_id, char* sem_name_buffer, int buffer_size);
void release_session_lock(int session_id, sem_t* session_sem);
void handle_session_logout(int socket_fd, int session_id, sem_t* session_sem);
void handle_unexpected_disconnect(int signum);

// --- Database & Logging ---
off_t find_customer_record_offset(int db_fd, int account_id);
off_t find_staff_record_offset(int db_fd, int employee_id);
off_t find_loan_record_offset(int db_fd, int loan_id);
void log_transaction(int account_id, const char* type, double amount, double new_balance);

// --- Global BuffFers ---
extern char g_read_buffer[1024];
extern char g_write_buffer[1024];

#endif // UTILS_H

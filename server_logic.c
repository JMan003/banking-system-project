/*
 * ========================================
 * server_logic.c
 * =Description: Implementation of all server-side
 * business logic (all user roles).
 * ========================================
 */

#include "server_logic.h"
#include "bank_storage.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <semaphore.h> 

// --- Global buffers are defined in server.c ---


// =======================================
// CUSTOMER ROLE
// =======================================

// --- Customer: Main Session ---
void handle_customer_session(int client_socket) {
    int logged_in_id = -1;
    sem_t* session_sem = NULL;
    char sem_name[50];
    
    // --- Login Loop ---
    while (logged_in_id == -1) {
        if (send_response(client_socket, "PROMPT", "Enter account ID: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        int account_id = atoi(g_read_buffer);
        if (account_id <= 0) continue;

        if (send_response(client_socket, "PROMPT_MASKED", "Enter PIN: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        
        session_sem = create_session_lock(account_id, sem_name, sizeof(sem_name));
        if (session_sem == NULL) {
            send_response(client_socket, "ERROR", "Server session error. Try again.");
            continue;
        }

        if (sem_trywait(session_sem) == -1) {
            if (errno == EAGAIN) {
                send_response(client_socket, "ERROR", "This account is already logged in elsewhere.");
            } else {
                perror("sem_trywait");
                send_response(client_socket, "ERROR", "Server lock error.");
            }
            sem_close(session_sem);
            continue;
        }
        
        if (login_customer(client_socket, account_id, g_read_buffer)) {
            logged_in_id = account_id;
            send_response(client_socket, "SUCCESS", "Login successful.");
            signal(SIGINT, handle_unexpected_disconnect);
            signal(SIGPIPE, handle_unexpected_disconnect);
        } else {
            // Use release_session_lock for a FAILED login
            release_session_lock(account_id, session_sem);
            send_response(client_socket, "ERROR", "Invalid ID, PIN, or inactive account.");
        }
    }

    // --- Main Menu Loop ---
    int choice = 0;
    while (choice != 9 && choice != 10) {
        const char* menu =
            "Customer Menu:\\n"
            "1. Deposit Money\\n2. Withdraw Money\\n3. View Balance\\n"
            "4. Transfer Funds\\n5. Apply for Loan\\n6. View Transaction History\\n"
            "7. Change PIN\\n8. Submit Feedback\\n9. Logout\\n10. Exit\\nChoice: ";
        
        if (send_response(client_socket, "PROMPT", menu) <= 0) { choice = 10; break; }
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 10; break; }
        choice = atoi(g_read_buffer);

        switch (choice) {
            case 1: handle_deposit(client_socket, logged_in_id); break;
            case 2: handle_withdrawal(client_socket, logged_in_id); break;
            case 3: handle_balance_check(client_socket, logged_in_id); break;
            case 4: handle_fund_transfer(client_socket, logged_in_id); break;
            case 5: handle_loan_request(client_socket, logged_in_id); break;
            case 6: handle_view_transactions(client_socket, logged_in_id); break;
            case 7: 
                handle_customer_password_change(client_socket, logged_in_id);
                choice = 9; // Force logout
                break;
            case 8: handle_submit_feedback(client_socket); break;
            case 9: printf("Customer %d selected logout.\n", logged_in_id); break;
            case 10: printf("Customer %d selected exit.\n", logged_in_id); break;
            default: send_response(client_socket, "ERROR", "Invalid choice.");
        }
    }

    // --- Cleanup ---
    if (choice == 10) { // Exit
        // Send logout message, which tells client to exit
        handle_session_logout(client_socket, logged_in_id, session_sem);
        close(client_socket);
        exit(0); // Terminate the child process
    } else { // Logout (choice 9) or password change
        // Just release the lock, don't send logout message
        release_session_lock(logged_in_id, session_sem);
        // Now the function will return to handle_client_connection,
        // which will send the main menu (the correct behavior).
    }
}

// --- Customer: Logic Implementation ---

int login_customer(int client_socket, int account_id, const char* pin) {
    struct CustomerAccount account;
    int db_fd = open(ACCOUNT_DB_FILE, O_RDONLY);
    if (db_fd == -1) {
        if (errno == ENOENT) { // First run, create file
             db_fd = open(ACCOUNT_DB_FILE, O_WRONLY | O_CREAT, 0644);
             if (db_fd != -1) close(db_fd);
        }
        return 0; // DB error or file just created
    }

    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) {
        close(db_fd);
        return 0; // Not found
    }

    lseek(db_fd, offset, SEEK_SET);
    read(db_fd, &account, sizeof(account));
    close(db_fd);

    return (strcmp(account.access_pin, pin) == 0 && account.is_active);
}

void handle_deposit(int client_socket, int account_id) {
    struct CustomerAccount account;
    struct flock lock;
    double amount;
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }

    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) { send_response(client_socket, "ERROR", "Account not found."); close(db_fd); return; }

    if (send_response(client_socket, "PROMPT", "Enter amount to deposit: ") <= 0) { close(db_fd); return; }
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { close(db_fd); return; }
    amount = atof(g_read_buffer);
    if (amount <= 0) { send_response(client_socket, "ERROR", "Invalid deposit amount."); close(db_fd); return; }
    
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = offset; lock.l_len = sizeof(struct CustomerAccount);

    if (fcntl(db_fd, F_SETLKW, &lock) == -1) { send_response(client_socket, "ERROR", "Failed to lock account. Try again."); close(db_fd); return; }

    lseek(db_fd, offset, SEEK_SET); read(db_fd, &account, sizeof(account));
    account.balance += amount;
    lseek(db_fd, offset, SEEK_SET); write(db_fd, &account, sizeof(account));
    lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);

    log_transaction(account_id, "DEPOSIT", amount, account.balance);
    snprintf(g_write_buffer, sizeof(g_write_buffer), "Deposit successful. New balance: %.2f", account.balance);
    send_response(client_socket, "SUCCESS", g_write_buffer);
}

void handle_withdrawal(int client_socket, int account_id) {
    struct CustomerAccount account;
    struct flock lock;
    double amount;
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) { send_response(client_socket, "ERROR", "Account not found."); close(db_fd); return; }

    if (send_response(client_socket, "PROMPT", "Enter amount to withdraw: ") <= 0) { close(db_fd); return; }
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { close(db_fd); return; }
    amount = atof(g_read_buffer);
    if (amount <= 0) { send_response(client_socket, "ERROR", "Invalid withdrawal amount."); close(db_fd); return; }
    
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = offset; lock.l_len = sizeof(struct CustomerAccount);

    if (fcntl(db_fd, F_SETLKW, &lock) == -1) { send_response(client_socket, "ERROR", "Failed to lock account. Try again."); close(db_fd); return; }

    lseek(db_fd, offset, SEEK_SET); read(db_fd, &account, sizeof(account));
    
    if (account.balance < amount) {
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Insufficient funds. Current balance: %.2f", account.balance);
        send_response(client_socket, "ERROR", g_write_buffer);
    } else {
        account.balance -= amount;
        lseek(db_fd, offset, SEEK_SET); write(db_fd, &account, sizeof(account));
        log_transaction(account_id, "WITHDRAWAL", -amount, account.balance);
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Withdrawal successful. New balance: %.2f", account.balance);
        send_response(client_socket, "SUCCESS", g_write_buffer);
    }
    
    lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
}

void handle_balance_check(int client_socket, int account_id) {
    struct CustomerAccount account;
    struct flock lock;
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDONLY);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) { send_response(client_socket, "ERROR", "Account not found."); close(db_fd); return; }
    
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK; lock.l_whence = SEEK_SET; lock.l_start = offset; lock.l_len = sizeof(struct CustomerAccount);

    if (fcntl(db_fd, F_SETLKW, &lock) == -1) { send_response(client_socket, "ERROR", "Failed to lock account. Try again."); close(db_fd); return; }

    lseek(db_fd, offset, SEEK_SET); read(db_fd, &account, sizeof(account));
    lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);

    snprintf(g_write_buffer, sizeof(g_write_buffer), "Current balance: %.2f", account.balance);
    send_response(client_socket, "SUCCESS", g_write_buffer);
}

void handle_customer_password_change(int client_socket, int account_id) {
    struct CustomerAccount account;
    struct flock lock;
    char new_pin[50];
    
    if (send_response(client_socket, "PROMPT_MASKED", "Enter new PIN: ") <= 0) return;
    if (read_line(client_socket, new_pin, sizeof(new_pin)) <= 0) return;
    if (strlen(new_pin) == 0) { send_response(client_socket, "ERROR", "PIN cannot be empty."); return; }
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) { send_response(client_socket, "ERROR", "Account not found."); close(db_fd); return; }

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = offset; lock.l_len = sizeof(struct CustomerAccount);
    
    if (fcntl(db_fd, F_SETLKW, &lock) == -1) { send_response(client_socket, "ERROR", "Failed to lock account. Try again."); close(db_fd); return; }

    lseek(db_fd, offset, SEEK_SET); read(db_fd, &account, sizeof(account));
    strncpy(account.access_pin, new_pin, sizeof(account.access_pin) - 1);
    account.access_pin[sizeof(account.access_pin) - 1] = '\0';
    lseek(db_fd, offset, SEEK_SET); write(db_fd, &account, sizeof(account));
    lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
    
    send_response(client_socket, "SUCCESS", "PIN changed successfully. You will be logged out.");
}

void handle_fund_transfer(int client_socket, int source_account_id) {
    struct CustomerAccount source_ac, dest_ac;
    struct flock lock_src, lock_dest;
    int dest_account_id;
    double amount;

    if (send_response(client_socket, "PROMPT", "Enter destination account ID: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    dest_account_id = atoi(g_read_buffer);
    
    if (send_response(client_socket, "PROMPT", "Enter amount to transfer: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    amount = atof(g_read_buffer);

    if (source_account_id == dest_account_id) { send_response(client_socket, "ERROR", "Cannot transfer to the same account."); return; }
    if (amount <= 0) { send_response(client_socket, "ERROR", "Invalid transfer amount."); return; }

    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset_src = find_customer_record_offset(db_fd, source_account_id);
    off_t offset_dest = find_customer_record_offset(db_fd, dest_account_id);

    if (offset_dest == -1) { send_response(client_socket, "ERROR", "Destination account not found."); close(db_fd); return; }

    memset(&lock_src, 0, sizeof(lock_src));
    lock_src.l_type = F_WRLCK; lock_src.l_whence = SEEK_SET; lock_src.l_start = offset_src; lock_src.l_len = sizeof(struct CustomerAccount);
    
    memset(&lock_dest, 0, sizeof(lock_dest));
    lock_dest.l_type = F_WRLCK; lock_dest.l_whence = SEEK_SET; lock_dest.l_start = offset_dest; lock_dest.l_len = sizeof(struct CustomerAccount);
    
    if (offset_src < offset_dest) { fcntl(db_fd, F_SETLKW, &lock_src); fcntl(db_fd, F_SETLKW, &lock_dest); }
    else { fcntl(db_fd, F_SETLKW, &lock_dest); fcntl(db_fd, F_SETLKW, &lock_src); }

    lseek(db_fd, offset_src, SEEK_SET); read(db_fd, &source_ac, sizeof(source_ac));
    lseek(db_fd, offset_dest, SEEK_SET); read(db_fd, &dest_ac, sizeof(dest_ac));

    if (source_ac.balance < amount) {
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Insufficient funds. Current balance: %.2f", source_ac.balance);
        send_response(client_socket, "ERROR", g_write_buffer);
    } else if (dest_ac.is_active == 0) {
        send_response(client_socket, "ERROR", "Destination account is inactive.");
    } else {
        source_ac.balance -= amount; dest_ac.balance += amount;
        lseek(db_fd, offset_src, SEEK_SET); write(db_fd, &source_ac, sizeof(source_ac));
        lseek(db_fd, offset_dest, SEEK_SET); write(db_fd, &dest_ac, sizeof(dest_ac));
        
        log_transaction(source_account_id, "TRANSFER_OUT", -amount, source_ac.balance);
        log_transaction(dest_account_id, "TRANSFER_IN", amount, dest_ac.balance);
        
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Transfer successful. New balance: %.2f", source_ac.balance);
        send_response(client_socket, "SUCCESS", g_write_buffer);
    }

    lock_src.l_type = F_UNLCK; lock_dest.l_type = F_UNLCK;
    fcntl(db_fd, F_SETLK, &lock_src); fcntl(db_fd, F_SETLK, &lock_dest);
    close(db_fd);
}

void handle_loan_request(int client_socket, int account_id) {
    struct LoanApplication loan;
    struct IDCounter counter;
    int loan_fd, counter_fd;
    double amount;

    if (send_response(client_socket, "PROMPT", "Enter loan amount: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    amount = atof(g_read_buffer);
    if (amount <= 0) { send_response(client_socket, "ERROR", "Invalid loan amount."); return; }

    counter_fd = open(LOAN_COUNTER_FILE, O_RDWR | O_CREAT, 0644);
    if (counter_fd == -1) { send_response(client_socket, "ERROR", "Server counter file error."); return; }
    
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(counter_fd, F_SETLKW, &lock);

    if (read(counter_fd, &counter, sizeof(counter)) <= 0) { counter.next_loan_id = 1; }
    loan.loan_id = counter.next_loan_id;
    counter.next_loan_id++;
    lseek(counter_fd, 0, SEEK_SET); write(counter_fd, &counter, sizeof(counter));
    lock.l_type = F_UNLCK; fcntl(counter_fd, F_SETLK, &lock);
    close(counter_fd);
    
    loan_fd = open(LOAN_DB_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (loan_fd == -1) { send_response(client_socket, "ERROR", "Server loan database error."); return; }
    
    loan.customer_account_id = account_id;
    loan.amount = amount;
    loan.status = 0; // 0 = Requested
    loan.assigned_to_employee_id = -1;

    lock.l_type = F_WRLCK; lock.l_start = 0;
    fcntl(loan_fd, F_SETLKW, &lock);
    write(loan_fd, &loan, sizeof(loan));
    lock.l_type = F_UNLCK; fcntl(loan_fd, F_SETLK, &lock);
    close(loan_fd);

    snprintf(g_write_buffer, sizeof(g_write_buffer), "Loan request #%d for %.2f submitted.", loan.loan_id, amount);
    send_response(client_socket, "SUCCESS", g_write_buffer);
}

void handle_view_transactions(int client_socket, int account_id) {
    struct Transaction log_entry;
    const int MAX_LOGS = 10;
    struct Transaction user_logs[MAX_LOGS];
    int log_count = 0;
    
    int log_fd = open(TRANSACTION_DB_FILE, O_RDONLY);
    if (log_fd == -1) {
        if (errno == ENOENT) { send_response(client_socket, "SUCCESS", "No transactions found."); return; }
        send_response(client_socket, "ERROR", "Server log database error.");
        return;
    }
    
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(log_fd, F_SETLKW, &lock);

    while (read(log_fd, &log_entry, sizeof(log_entry)) == sizeof(log_entry)) {
        if (log_entry.account_id == account_id) {
            user_logs[log_count % MAX_LOGS] = log_entry;
            log_count++;
        }
    }
    
    lock.l_type = F_UNLCK; fcntl(log_fd, F_SETLK, &lock);
    close(log_fd);

    if (log_count == 0) { send_response(client_socket, "SUCCESS", "No transactions found."); return; }

    bzero(g_write_buffer, sizeof(g_write_buffer));
    strcat(g_write_buffer, "Last Transactions:\\n");
    
    int start = (log_count < MAX_LOGS) ? 0 : (log_count % MAX_LOGS);
    int num_to_print = (log_count < MAX_LOGS) ? log_count : MAX_LOGS;

    for (int i = 0; i < num_to_print; i++) {
        struct Transaction* entry = &user_logs[(start + i) % MAX_LOGS];
        char line[200];
        snprintf(line, sizeof(line), "[%s] %s | Balance: %.2f\\n",
                 entry->timestamp, entry->description, entry->resulting_balance);
        if (strlen(g_write_buffer) + strlen(line) < sizeof(g_write_buffer) - 1) {
            strcat(g_write_buffer, line);
        } else {
            break;
        }
    }
    send_response(client_socket, "SUCCESS", g_write_buffer);
}

void handle_submit_feedback(int client_socket) {
    if (send_response(client_socket, "PROMPT", "Enter your feedback: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    if (strlen(g_read_buffer) == 0) { send_response(client_socket, "ERROR", "Feedback cannot be empty."); return; }

    int fb_fd = open(FEEDBACK_DB_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fb_fd == -1) { send_response(client_socket, "ERROR", "Server feedback database error."); return; }

    struct FeedbackEntry feedback;
    strncpy(feedback.feedback_text, g_read_buffer, sizeof(feedback.feedback_text) - 1);
    feedback.feedback_text[sizeof(feedback.feedback_text) - 1] = '\0';
    
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(fb_fd, F_SETLKW, &lock);
    write(fb_fd, &feedback, sizeof(feedback));
    lock.l_type = F_UNLCK; fcntl(fb_fd, F_SETLK, &lock);
    close(fb_fd);

    send_response(client_socket, "SUCCESS", "Thank you for your feedback!");
}


// =======================================
// STAFF ROLE
// =======================================

// --- Staff: Main Session ---
void handle_staff_session(int client_socket) {
    int logged_in_id = -1;
    sem_t* session_sem = NULL;
    char sem_name[50];
    
    // --- Login Loop ---
    while (logged_in_id == -1) {
        if (send_response(client_socket, "PROMPT", "Enter Employee ID: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        int employee_id = atoi(g_read_buffer);
        if (employee_id <= 0) continue;

        if (send_response(client_socket, "PROMPT_MASKED", "Enter password: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        
        session_sem = create_session_lock(employee_id, sem_name, sizeof(sem_name));
        if (session_sem == NULL) {
            send_response(client_socket, "ERROR", "Server session error. Try again.");
            continue;
        }

        if (sem_trywait(session_sem) == -1) {
            if (errno == EAGAIN) {
                send_response(client_socket, "ERROR", "This ID is already logged in elsewhere.");
            } else {
                perror("sem_trywait");
                send_response(client_socket, "ERROR", "Server lock error.");
            }
            sem_close(session_sem);
            continue;
        }
        
        // Use 1 for "Staff" role
        if (login_staff(client_socket, employee_id, g_read_buffer, 1)) {
            logged_in_id = employee_id;
            send_response(client_socket, "SUCCESS", "Login successful.");
            signal(SIGINT, handle_unexpected_disconnect);
            signal(SIGPIPE, handle_unexpected_disconnect);
        } else {
            // Use release_session_lock for a FAILED login
            release_session_lock(employee_id, session_sem);
            send_response(client_socket, "ERROR", "Invalid ID, password, or role.");
        }
    }

    // --- Main Menu Loop ---
    int choice = 0;
    while (choice != 7 && choice != 8) {
        const char* menu =
            "Employee Menu:\\n"
            "1. Add New Customer\\n2. Modify Customer Details\\n3. Process Loan Applications\\n"
            "4. View Assigned Loan Applications\\n5. View Customer Transactions\\n"
            "6. Change Password\\n7. Logout\\n8. Exit\\nChoice: ";
        
        if (send_response(client_socket, "PROMPT", menu) <= 0) { choice = 8; break; }
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 8; break; }
        choice = atoi(g_read_buffer);

        switch (choice) {
            case 1: handle_create_customer(client_socket); break;
            case 2: handle_modify_user_details(client_socket, 1); break; // 1 = Customer
            case 3: handle_process_loan(client_socket, logged_in_id); break;
            case 4: handle_view_assigned_loans(client_socket, logged_in_id); break;
            case 5: {
                if (send_response(client_socket, "PROMPT", "Enter Account ID to view: ") <= 0) { choice = 8; break; }
                if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 8; break; }
                handle_view_transactions(client_socket, atoi(g_read_buffer));
                break;
            }
            case 6:
                handle_staff_password_change(client_socket, logged_in_id);
                choice = 7; // Force logout
                break;
            case 7: printf("Staff %d selected logout.\n", logged_in_id); break;
            case 8: printf("Staff %d selected exit.\n", logged_in_id); break;
            default: send_response(client_socket, "ERROR", "Invalid choice.");
        }
    }

    // --- Cleanup ---
    if (choice == 8) { // Exit
        handle_session_logout(client_socket, logged_in_id, session_sem);
        close(client_socket);
        exit(0); 
    } else { // Logout (choice 7) or password change
        release_session_lock(logged_in_id, session_sem);
    }
}

// --- Staff: Logic Implementation ---

int login_staff(int client_socket, int employee_id, const char* pin, int role_required) {
    struct EmployeeRecord staff;
    int db_fd = open(STAFF_DB_FILE, O_RDONLY);
    if (db_fd == -1) {
         if (errno == ENOENT) {
             db_fd = open(STAFF_DB_FILE, O_WRONLY | O_CREAT, 0644);
             if (db_fd != -1) close(db_fd);
         }
        return 0; // DB error or file just created
    }

    off_t offset = find_staff_record_offset(db_fd, employee_id);
    if (offset == -1) {
        close(db_fd);
        return 0; // Not found
    }

    lseek(db_fd, offset, SEEK_SET);
    read(db_fd, &staff, sizeof(staff));
    close(db_fd);

    return (strcmp(staff.login_pass, pin) == 0 && staff.role == role_required);
}

void handle_create_customer(int client_socket) {
    struct CustomerAccount new_account, temp_account;
    
    if (send_response(client_socket, "PROMPT", "Enter new Customer Account ID: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    new_account.account_id = atoi(g_read_buffer);
    
    if (send_response(client_socket, "PROMPT", "Enter Customer Name: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    strncpy(new_account.owner_name, g_read_buffer, sizeof(new_account.owner_name) - 1);
    
    if (send_response(client_socket, "PROMPT_MASKED", "Enter initial PIN: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    strncpy(new_account.access_pin, g_read_buffer, sizeof(new_account.access_pin) - 1);

    if (send_response(client_socket, "PROMPT", "Enter Opening Balance: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    new_account.balance = atof(g_read_buffer);
    if (new_account.balance < 0) new_account.balance = 0;
    
    new_account.is_active = 1; // Active by default
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR | O_CREAT, 0644);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(db_fd, F_SETLKW, &lock);
    
    int duplicate = 0;
    lseek(db_fd, 0, SEEK_SET);
    while (read(db_fd, &temp_account, sizeof(temp_account)) == sizeof(temp_account)) {
        if (temp_account.account_id == new_account.account_id) {
            duplicate = 1;
            break;
        }
    }
    
    if (duplicate) {
        send_response(client_socket, "ERROR", "Account ID already exists.");
    } else {
        lseek(db_fd, 0, SEEK_END);
        write(db_fd, &new_account, sizeof(new_account));
        log_transaction(new_account.account_id, "OPENING_BALANCE", new_account.balance, new_account.balance);
        send_response(client_socket, "SUCCESS", "Customer account created successfully.");
    }
    
    lock.l_type = F_UNLCK;
    fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
}

void handle_process_loan(int client_socket, int employee_id) {
    struct LoanApplication loan;
    struct CustomerAccount account;
    struct flock lock_loan, lock_acct;
    int loan_id, choice;
    
    if (send_response(client_socket, "PROMPT", "Enter Loan ID to process: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    loan_id = atoi(g_read_buffer);
    
    int loan_fd = open(LOAN_DB_FILE, O_RDWR);
    int acct_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (loan_fd == -1 || acct_fd == -1) {
        send_response(client_socket, "ERROR", "Server database error.");
        if (loan_fd != -1) close(loan_fd);
        if (acct_fd != -1) close(acct_fd);
        return;
    }
    
    off_t offset_loan = find_loan_record_offset(loan_fd, loan_id);
    if (offset_loan == -1) {
        send_response(client_socket, "ERROR", "Loan ID not found.");
        close(loan_fd); close(acct_fd); return;
    }
    
    lseek(loan_fd, offset_loan, SEEK_SET);
    read(loan_fd, &loan, sizeof(loan));
    
    if (loan.assigned_to_employee_id != employee_id) {
        send_response(client_socket, "ERROR", "This loan is not assigned to you.");
        close(loan_fd); close(acct_fd); return;
    }
    if (loan.status != 1) { // 1 = Assigned/Pending
        send_response(client_socket, "ERROR", "This loan is not pending processing.");
        close(loan_fd); close(acct_fd); return;
    }
    
    off_t offset_acct = find_customer_record_offset(acct_fd, loan.customer_account_id);
    if (offset_acct == -1) {
        send_response(client_socket, "ERROR", "CRITICAL: Customer account for this loan not found.");
        close(loan_fd); close(acct_fd); return;
    }
    
    memset(&lock_loan, 0, sizeof(lock_loan));
    lock_loan.l_type = F_WRLCK; lock_loan.l_whence = SEEK_SET; lock_loan.l_start = offset_loan; lock_loan.l_len = sizeof(struct LoanApplication);
    
    memset(&lock_acct, 0, sizeof(lock_acct));
    lock_acct.l_type = F_WRLCK; lock_acct.l_whence = SEEK_SET; lock_acct.l_start = offset_acct; lock_acct.l_len = sizeof(struct CustomerAccount);
    
    fcntl(loan_fd, F_SETLKW, &lock_loan);
    fcntl(acct_fd, F_SETLKW, &lock_acct);

    lseek(loan_fd, offset_loan, SEEK_SET); read(loan_fd, &loan, sizeof(loan));
    lseek(acct_fd, offset_acct, SEEK_SET); read(acct_fd, &account, sizeof(account));
    
    if (loan.status != 1) {
        send_response(client_socket, "ERROR", "Loan status changed before processing. Aborting.");
    } else {
        snprintf(g_write_buffer, sizeof(g_write_buffer),
            "Processing Loan #%d for Acct %d (%s).\\nAmount: %.2f. Balance: %.2f\\n"
            "1. Approve\\n2. Reject\\nChoice: ",
            loan.loan_id, account.account_id, account.owner_name, loan.amount, account.balance);
        
        if (send_response(client_socket, "PROMPT", g_write_buffer) <= 0) goto cleanup_loan_proc;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_loan_proc;
        choice = atoi(g_read_buffer);

        if (choice == 1) { // Approve
            account.balance += loan.amount;
            loan.status = 2; // Approved
            lseek(acct_fd, offset_acct, SEEK_SET);
            write(acct_fd, &account, sizeof(account));
            log_transaction(account.account_id, "LOAN_APPROVED", loan.amount, account.balance);
            send_response(client_socket, "SUCCESS", "Loan Approved.");
        } else if (choice == 2) { // Reject
            loan.status = 3; // Rejected
            send_response(client_socket, "SUCCESS", "Loan Rejected.");
        } else {
            send_response(client_socket, "ERROR", "Invalid choice. No action taken.");
        }
        
        if (choice == 1 || choice == 2) {
            lseek(loan_fd, offset_loan, SEEK_SET);
            write(loan_fd, &loan, sizeof(loan));
        }
    }
    
cleanup_loan_proc:
    lock_loan.l_type = F_UNLCK; fcntl(loan_fd, F_SETLK, &lock_loan);
    lock_acct.l_type = F_UNLCK; fcntl(acct_fd, F_SETLK, &lock_acct);
    close(loan_fd);
    close(acct_fd);
}

void handle_view_assigned_loans(int client_socket, int employee_id) {
    struct LoanApplication loan;
    int loan_fd = open(LOAN_DB_FILE, O_RDONLY);
    if (loan_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(loan_fd, F_SETLKW, &lock);
    
    int found = 0;
    bzero(g_write_buffer, sizeof(g_write_buffer));
    strcat(g_write_buffer, "Assigned Pending Loans:\\n");

    while (read(loan_fd, &loan, sizeof(loan)) == sizeof(loan)) {
        if (loan.assigned_to_employee_id == employee_id && loan.status == 1) { // 1 = Assigned
            char line[100];
            snprintf(line, sizeof(line), "-> Loan #%d | Acct: %d | Amount: %.2f\\n",
                     loan.loan_id, loan.customer_account_id, loan.amount);
            
            if (strlen(g_write_buffer) + strlen(line) < sizeof(g_write_buffer) - 50) {
                 strcat(g_write_buffer, line);
            }
            found = 1;
        }
    }
    
    lock.l_type = F_UNLCK;
    fcntl(loan_fd, F_SETLK, &lock);
    close(loan_fd);
    
    if (!found) {
        send_response(client_socket, "SUCCESS", "No pending loans assigned to you.");
    } else {
        send_response(client_socket, "SUCCESS", g_write_buffer);
    }
}


// =======================================
// MANAGER ROLE
// =======================================

// --- Manager: Main Session ---
void handle_manager_session(int client_socket) {
    int logged_in_id = -1;
    sem_t* session_sem = NULL;
    char sem_name[50];
    
    // --- Login Loop ---
    while (logged_in_id == -1) {
        if (send_response(client_socket, "PROMPT", "Enter Manager ID: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        int employee_id = atoi(g_read_buffer);
        if (employee_id <= 0) continue;

        if (send_response(client_socket, "PROMPT_MASKED", "Enter password: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        
        session_sem = create_session_lock(employee_id, sem_name, sizeof(sem_name));
        if (session_sem == NULL) {
            send_response(client_socket, "ERROR", "Server session error. Try again.");
            continue;
        }

        if (sem_trywait(session_sem) == -1) {
            if (errno == EAGAIN) {
                send_response(client_socket, "ERROR", "This ID is already logged in elsewhere.");
            } else {
                perror("sem_trywait");
                send_response(client_socket, "ERROR", "Server lock error.");
            }
            sem_close(session_sem);
            continue;
        }
        
        // Use 0 for "Manager" role
        if (login_staff(client_socket, employee_id, g_read_buffer, 0)) {
            logged_in_id = employee_id;
            send_response(client_socket, "SUCCESS", "Login successful.");
            signal(SIGINT, handle_unexpected_disconnect);
            signal(SIGPIPE, handle_unexpected_disconnect);
        } else {
            // Use release_session_lock for a FAILED login
            release_session_lock(employee_id, session_sem);
            send_response(client_socket, "ERROR", "Invalid ID, password, or role.");
        }
    }

    // --- Main Menu Loop ---
    int choice = 0;
    while (choice != 5 && choice != 6) {
        const char* menu =
            "Manager Menu:\\n"
            "1. Activate/Deactivate Customer Accounts\\n2. Assign Loan Applications\\n"
            "3. Review Customer Feedback\\n4. Change Password\\n"
            "5. Logout\\n6. Exit\\nChoice: ";
        
        if (send_response(client_socket, "PROMPT", menu) <= 0) { choice = 6; break; }
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 6; break; }
        choice = atoi(g_read_buffer);

        switch (choice) {
            case 1: handle_set_account_status(client_socket); break;
            case 2: handle_assign_loan(client_socket); break;
            case 3: handle_review_feedback(client_socket); break;
            case 4:
                handle_staff_password_change(client_socket, logged_in_id);
                choice = 5; // Force logout
                break;
            case 5: printf("Manager %d selected logout.\n", logged_in_id); break;
            case 6: printf("Manager %d selected exit.\n", logged_in_id); break;
            default: send_response(client_socket, "ERROR", "Invalid choice.");
        }
    }

    // --- Cleanup ---
    if (choice == 6) { // Exit
        handle_session_logout(client_socket, logged_in_id, session_sem);
        close(client_socket);
        exit(0);
    } else { // Logout (choice 5) or password change
        release_session_lock(logged_in_id, session_sem);
    }
}

// --- Manager: Logic Implementation ---

void handle_set_account_status(int client_socket) {
    struct CustomerAccount account;
    int account_id, choice;
    
    if (send_response(client_socket, "PROMPT", "Enter Customer Account ID: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    account_id = atoi(g_read_buffer);
    
    int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset = find_customer_record_offset(db_fd, account_id);
    if (offset == -1) {
        send_response(client_socket, "ERROR", "Account not found.");
        close(db_fd); return;
    }
    
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct CustomerAccount), getpid()};
    fcntl(db_fd, F_SETLKW, &lock);
    
    lseek(db_fd, offset, SEEK_SET);
    read(db_fd, &account, sizeof(account));
    
    snprintf(g_write_buffer, sizeof(g_write_buffer),
        "Account %d (%s) is currently: %s\\n"
        "1. Activate\\n2. Deactivate\\nChoice: ",
        account_id, account.owner_name, account.is_active ? "ACTIVE" : "INACTIVE");
    
    if (send_response(client_socket, "PROMPT", g_write_buffer) <= 0) goto cleanup_set_status;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_set_status;
    choice = atoi(g_read_buffer);
    
    if (choice == 1) {
        account.is_active = 1;
        lseek(db_fd, offset, SEEK_SET);
        write(db_fd, &account, sizeof(account));
        send_response(client_socket, "SUCCESS", "Account activated.");
    } else if (choice == 2) {
        account.is_active = 0;
        lseek(db_fd, offset, SEEK_SET);
        write(db_fd, &account, sizeof(account));
        send_response(client_socket, "SUCCESS", "Account deactivated.");
    } else {
        send_response(client_socket, "ERROR", "Invalid choice. No action taken.");
    }

cleanup_set_status:
    lock.l_type = F_UNLCK;
    fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
}

void handle_assign_loan(int client_socket) {
    struct LoanApplication loan;
    int loan_id, employee_id;
    
    int loan_fd = open(LOAN_DB_FILE, O_RDWR); // RDWR for read then write
    if (loan_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(loan_fd, F_SETLKW, &lock);
    
    int found = 0;
    bzero(g_write_buffer, sizeof(g_write_buffer));
    strcat(g_write_buffer, "Unassigned Loan Requests (Status 0):\\n");

    while (read(loan_fd, &loan, sizeof(loan)) == sizeof(loan)) {
        if (loan.status == 0) { // 0 = Requested
            char line[100];
            snprintf(line, sizeof(line), "-> Loan #%d | Acct: %d | Amount: %.2f\\n",
                     loan.loan_id, loan.customer_account_id, loan.amount);
            if (strlen(g_write_buffer) + strlen(line) < sizeof(g_write_buffer) - 50) {
                 strcat(g_write_buffer, line);
            }
            found = 1;
        }
    }
    lock.l_type = F_UNLCK; fcntl(loan_fd, F_SETLK, &lock);
    
    if (!found) {
        send_response(client_socket, "SUCCESS", "No unassigned loans found.");
        close(loan_fd); return;
    }
    
    if (send_response(client_socket, "SUCCESS", g_write_buffer) <= 0) { close(loan_fd); return; }
    
    if (send_response(client_socket, "PROMPT", "Enter Loan ID to assign: ") <= 0) { close(loan_fd); return; }
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { close(loan_fd); return; }
    loan_id = atoi(g_read_buffer);
    
    if (send_response(client_socket, "PROMPT", "Enter Employee ID to assign to: ") <= 0) { close(loan_fd); return; }
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { close(loan_fd); return; }
    employee_id = atoi(g_read_buffer);
    
    off_t offset = find_loan_record_offset(loan_fd, loan_id);
    if (offset == -1) {
        send_response(client_socket, "ERROR", "Loan ID not found.");
        close(loan_fd); return;
    }
    
    lock.l_type = F_WRLCK; lock.l_start = offset; lock.l_len = sizeof(struct LoanApplication);
    fcntl(loan_fd, F_SETLKW, &lock);
    
    lseek(loan_fd, offset, SEEK_SET);
    read(loan_fd, &loan, sizeof(loan));
    
    if (loan.status != 0) {
        send_response(client_socket, "ERROR", "Loan was already assigned or processed.");
    } else {
        loan.status = 1; // 1 = Assigned
        loan.assigned_to_employee_id = employee_id;
        
        lseek(loan_fd, offset, SEEK_SET);
        write(loan_fd, &loan, sizeof(loan));
        
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Loan #%d assigned to Employee #%d.", loan_id, employee_id);
        send_response(client_socket, "SUCCESS", g_write_buffer);
    }
    
    lock.l_type = F_UNLCK; fcntl(loan_fd, F_SETLK, &lock);
    close(loan_fd);
}

void handle_review_feedback(int client_socket) {
    struct FeedbackEntry feedback;
    int fb_fd = open(FEEDBACK_DB_FILE, O_RDONLY);
    if (fb_fd == -1) {
        if (errno == ENOENT) { send_response(client_socket, "SUCCESS", "No feedback submitted yet."); return; }
        send_response(client_socket, "ERROR", "Server database error.");
        return;
    }
    
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(fb_fd, F_SETLKW, &lock);
    
    bzero(g_write_buffer, sizeof(g_write_buffer));
    strcat(g_write_buffer, "All Customer Feedback:\\n");
    int count = 0;
    
    while (read(fb_fd, &feedback, sizeof(feedback)) == sizeof(feedback)) {
        char line[300];
        snprintf(line, sizeof(line), "-> %s\\n", feedback.feedback_text);
        if (strlen(g_write_buffer) + strlen(line) < sizeof(g_write_buffer) - 50) {
             strcat(g_write_buffer, line);
        } else {
             strcat(g_write_buffer, "...(more entries truncated)...\\n");
             break;
        }
        count++;
    }
    
    lock.l_type = F_UNLCK; fcntl(fb_fd, F_SETLK, &lock);
    close(fb_fd);
    
    if (count == 0) {
        send_response(client_socket, "SUCCESS", "No feedback submitted yet.");
    } else {
        send_response(client_socket, "SUCCESS", g_write_buffer);
    }
}


// =======================================
// ADMIN ROLE
// =======================================

// --- Admin: Main Session ---
void handle_admin_session(int client_socket) {
    int logged_in = 0;
    
    // --- Login Loop ---
    while (!logged_in) {
        if (send_response(client_socket, "PROMPT_MASKED", "Enter Admin Password: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        
        if (login_admin(client_socket, g_read_buffer)) {
            logged_in = 1;
            send_response(client_socket, "SUCCESS", "Admin login successful.");
        } else {
            send_response(client_socket, "ERROR", "Invalid password.");
        }
    }

    // --- Main Menu Loop ---
    int choice = 0;
    while (choice != 5) {
        const char* menu =
            "Admin Menu:\\n"
            "1. Add New Bank Employee/Manager\\n2. Modify Customer/Employee Details\\n"
            "3. Manage User Roles\\n4. Change Admin Password\\n"
            "5. Logout\\nChoice: ";
        
        if (send_response(client_socket, "PROMPT", menu) <= 0) { choice = 5; break; }
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 5; break; }
        choice = atoi(g_read_buffer);

        switch (choice) {
            case 1: handle_create_staff(client_socket); break;
            case 2: {
                if (send_response(client_socket, "PROMPT", "1. Modify Customer\\n2. Modify Employee\\nChoice: ") <= 0) { choice = 5; break; }
                if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) { choice = 5; break; }
                handle_modify_user_details(client_socket, atoi(g_read_buffer));
                break;
            }
            case 3: handle_update_staff_role(client_socket); break;
            case 4: handle_change_admin_pass(client_socket); break;
            case 5: printf("Admin selected logout.\n"); break;
            default: send_response(client_socket, "ERROR", "Invalid choice.");
        }
    }
    
    // Admin logout is simple: just send the message. No session lock to clean up.
    send_response(client_socket, "LOGOUT", "Logged out successfully.");
}

// --- Admin: Logic Implementation ---

int login_admin(int client_socket, const char* pass) {
    char stored_pass[50];
    const char* default_pass = "root123";
    
    int fd = open(ADMIN_PASS_FILE, O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            fd = open(ADMIN_PASS_FILE, O_WRONLY | O_CREAT, 0600);
            if (fd == -1) {
                perror("Admin: Failed to create pass file"); return 0;
            }
            write(fd, default_pass, strlen(default_pass));
            close(fd);
            return (strcmp(pass, default_pass) == 0);
        } else {
            perror("Admin: Failed to open pass file");
            return 0;
        }
    }
    
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(fd, F_SETLKW, &lock);
    
    bzero(stored_pass, sizeof(stored_pass));
    read(fd, stored_pass, sizeof(stored_pass) - 1);
    
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);
    
    return (strcmp(pass, stored_pass) == 0);
}

void handle_create_staff(int client_socket) {
    struct EmployeeRecord new_staff, temp_staff;
    
    if (send_response(client_socket, "PROMPT", "Enter new Employee ID: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    new_staff.employee_id = atoi(g_read_buffer);
    
    if (send_response(client_socket, "PROMPT", "Enter First Name: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    strncpy(new_staff.first_name, g_read_buffer, sizeof(new_staff.first_name) - 1);
    
    if (send_response(client_socket, "PROMPT", "Enter Last Name: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    strncpy(new_staff.last_name, g_read_buffer, sizeof(new_staff.last_name) - 1);

    if (send_response(client_socket, "PROMPT_MASKED", "Enter initial password: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    strncpy(new_staff.login_pass, g_read_buffer, sizeof(new_staff.login_pass) - 1);

    if (send_response(client_socket, "PROMPT", "Enter Role (0=Manager, 1=Employee): ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    new_staff.role = (atoi(g_read_buffer) == 0) ? 0 : 1; // Default to 1 (Employee)

    int db_fd = open(STAFF_DB_FILE, O_RDWR | O_CREAT, 0644);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(db_fd, F_SETLKW, &lock);
    
    int duplicate = 0;
    lseek(db_fd, 0, SEEK_SET);
    while (read(db_fd, &temp_staff, sizeof(temp_staff)) == sizeof(temp_staff)) {
        if (temp_staff.employee_id == new_staff.employee_id) {
            duplicate = 1;
            break;
        }
    }
    
    if (duplicate) {
        send_response(client_socket, "ERROR", "Employee ID already exists.");
    } else {
        lseek(db_fd, 0, SEEK_END);
        write(db_fd, &new_staff, sizeof(new_staff));
        send_response(client_socket, "SUCCESS", "Staff account created successfully.");
    }
    
    lock.l_type = F_UNLCK;
    fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
}

void handle_update_staff_role(int client_socket) {
    struct EmployeeRecord staff;
    int employee_id, choice;
    
    if (send_response(client_socket, "PROMPT", "Enter Employee ID: ") <= 0) return;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
    employee_id = atoi(g_read_buffer);
    
    int db_fd = open(STAFF_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
    
    off_t offset = find_staff_record_offset(db_fd, employee_id);
    if (offset == -1) {
        send_response(client_socket, "ERROR", "Employee not found.");
        close(db_fd); return;
    }
    
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct EmployeeRecord), getpid()};
    fcntl(db_fd, F_SETLKW, &lock);
    
    lseek(db_fd, offset, SEEK_SET);
    read(db_fd, &staff, sizeof(staff));
    
    snprintf(g_write_buffer, sizeof(g_write_buffer),
        "Employee %d (%s) is currently: %s\\n"
        "1. Make Employee\\n0. Make Manager\\nChoice: ",
        employee_id, staff.first_name, (staff.role == 0) ? "Manager" : "Employee");
    
    if (send_response(client_socket, "PROMPT", g_write_buffer) <= 0) goto cleanup_update_role;
    if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_update_role;
    choice = atoi(g_read_buffer);
    
    if (choice == 0) {
        staff.role = 0; // Manager
        lseek(db_fd, offset, SEEK_SET); write(db_fd, &staff, sizeof(staff));
        send_response(client_socket, "SUCCESS", "Role updated to Manager.");
    } else if (choice == 1) {
        staff.role = 1; // Employee
        lseek(db_fd, offset, SEEK_SET); write(db_fd, &staff, sizeof(staff));
        send_response(client_socket, "SUCCESS", "Role updated to Employee.");
    } else {
        send_response(client_socket, "ERROR", "Invalid choice. No action taken.");
    }

cleanup_update_role:
    lock.l_type = F_UNLCK;
    fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
}

void handle_change_admin_pass(int client_socket) {
    char new_pass[50];
    
    if (send_response(client_socket, "PROMPT_MASKED", "Enter new admin password: ") <= 0) return;
    if (read_line(client_socket, new_pass, sizeof(new_pass)) <= 0) return;
    if (strlen(new_pass) == 0) { send_response(client_socket, "ERROR", "Password cannot be empty."); return; }
    
    int fd = open(ADMIN_PASS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        send_response(client_socket, "ERROR", "Server failed to open pass file.");
        return;
    }
    
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(fd, F_SETLKW, &lock);
    
    write(fd, new_pass, strlen(new_pass));
    
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);
    
    send_response(client_socket, "SUCCESS", "Admin password changed.");
}


// =======================================
// SHARED LOGIC
// =======================================

void handle_modify_user_details(int client_socket, int modify_type) {
    if (modify_type == 1) {
        // --- Modify Customer ---
        struct CustomerAccount account;
        int account_id;
        
        if (send_response(client_socket, "PROMPT", "Enter Customer Account ID: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        account_id = atoi(g_read_buffer);
        
        int db_fd = open(ACCOUNT_DB_FILE, O_RDWR);
        if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
        
        off_t offset = find_customer_record_offset(db_fd, account_id);
        if (offset == -1) {
            send_response(client_socket, "ERROR", "Account not found.");
            close(db_fd); return;
        }
        
        struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct CustomerAccount), getpid()};
        fcntl(db_fd, F_SETLKW, &lock);
        
        lseek(db_fd, offset, SEEK_SET); read(db_fd, &account, sizeof(account));
        
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Current name: %s. Enter new name: ", account.owner_name);
        if (send_response(client_socket, "PROMPT", g_write_buffer) <= 0) goto cleanup_mod_cust;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_mod_cust;
        
        strncpy(account.owner_name, g_read_buffer, sizeof(account.owner_name) - 1);
        lseek(db_fd, offset, SEEK_SET); write(db_fd, &account, sizeof(account));
        send_response(client_socket, "SUCCESS", "Customer name updated.");
        
    cleanup_mod_cust:
        lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
        close(db_fd);
        
    } else if (modify_type == 2) {
        // --- Modify Staff ---
        struct EmployeeRecord staff;
        int employee_id;
        
        if (send_response(client_socket, "PROMPT", "Enter Employee ID: ") <= 0) return;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) return;
        employee_id = atoi(g_read_buffer);
        
        int db_fd = open(STAFF_DB_FILE, O_RDWR);
        if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return; }
        
        off_t offset = find_staff_record_offset(db_fd, employee_id);
        if (offset == -1) {
            send_response(client_socket, "ERROR", "Employee not found.");
            close(db_fd); return;
        }
        
        struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct EmployeeRecord), getpid()};
        fcntl(db_fd, F_SETLKW, &lock);
        
        lseek(db_fd, offset, SEEK_SET); read(db_fd, &staff, sizeof(staff));
        
        snprintf(g_write_buffer, sizeof(g_write_buffer), "Current name: %s %s. Enter new First Name: ", staff.first_name, staff.last_name);
        if (send_response(client_socket, "PROMPT", g_write_buffer) <= 0) goto cleanup_mod_staff;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_mod_staff;
        strncpy(staff.first_name, g_read_buffer, sizeof(staff.first_name) - 1);
        
        if (send_response(client_socket, "PROMPT", "Enter new Last Name: ") <= 0) goto cleanup_mod_staff;
        if (read_line(client_socket, g_read_buffer, sizeof(g_read_buffer)) <= 0) goto cleanup_mod_staff;
        strncpy(staff.last_name, g_read_buffer, sizeof(staff.last_name) - 1);

        lseek(db_fd, offset, SEEK_SET); write(db_fd, &staff, sizeof(staff));
        send_response(client_socket, "SUCCESS", "Staff name updated.");
        
    cleanup_mod_staff:
        lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
        close(db_fd);
        
    } else {
        send_response(client_socket, "ERROR", "Invalid modification type.");
    }
}

int handle_staff_password_change(int client_socket, int employee_id) {
    struct EmployeeRecord staff;
    char new_pass[50];
    
    if (send_response(client_socket, "PROMPT_MASKED", "Enter new password: ") <= 0) return 0;
    if (read_line(client_socket, new_pass, sizeof(new_pass)) <= 0) return 0;
    if (strlen(new_pass) == 0) {
        send_response(client_socket, "ERROR", "Password cannot be empty.");
        return 0;
    }
    
    int db_fd = open(STAFF_DB_FILE, O_RDWR);
    if (db_fd == -1) { send_response(client_socket, "ERROR", "Server database error."); return 0; }
    
    off_t offset = find_staff_record_offset(db_fd, employee_id);
    if (offset == -1) {
        send_response(client_socket, "ERROR", "Employee not found.");
        close(db_fd); return 0;
    }
    
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct EmployeeRecord), getpid()};
    fcntl(db_fd, F_SETLKW, &lock);
    
    lseek(db_fd, offset, SEEK_SET); read(db_fd, &staff, sizeof(staff));
    strncpy(staff.login_pass, new_pass, sizeof(staff.login_pass) - 1);
    staff.login_pass[sizeof(staff.login_pass) - 1] = '\0';
    lseek(db_fd, offset, SEEK_SET); write(db_fd, &staff, sizeof(staff));
    
    lock.l_type = F_UNLCK; fcntl(db_fd, F_SETLK, &lock);
    close(db_fd);
    
    send_response(client_socket, "SUCCESS", "Password changed. You will be logged out.");
    return 1; // Success
}

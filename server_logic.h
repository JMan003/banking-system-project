/*
 * ========================================
 * server_logic.h
 * =Description: Prototypes for all server-side
 * business logic (all user roles).
 * ========================================
 */

#ifndef SERVER_LOGIC_H
#define SERVER_LOGIC_H

// --- Main Session Handlers ---
void handle_customer_session(int client_socket);
void handle_staff_session(int client_socket);
void handle_manager_session(int client_socket);
void handle_admin_session(int client_socket);

// --- Shared Logic (Staff/Admin) ---
void handle_modify_user_details(int client_socket, int modify_type);
int handle_staff_password_change(int client_socket, int employee_id);

// --- Customer-Specific Logic ---
int login_customer(int client_socket, int account_id, const char* pin);
void handle_deposit(int client_socket, int account_id);
void handle_withdrawal(int client_socket, int account_id);
void handle_balance_check(int client_socket, int account_id);
void handle_customer_password_change(int client_socket, int account_id);
void handle_fund_transfer(int client_socket, int source_account_id);
void handle_loan_request(int client_socket, int account_id);
void handle_view_transactions(int client_socket, int account_id);
void handle_submit_feedback(int client_socket);

// --- Staff-Specific Logic ---
int login_staff(int client_socket, int employee_id, const char* pin, int role_required);
void handle_create_customer(int client_socket);
void handle_process_loan(int client_socket, int employee_id);
void handle_view_assigned_loans(int client_socket, int employee_id);

// --- Manager-Specific Logic ---
void handle_set_account_status(int client_socket);
void handle_assign_loan(int client_socket);
void handle_review_feedback(int client_socket);

// --- Admin-Specific Logic ---
int login_admin(int client_socket, const char* pass);
void handle_create_staff(int client_socket);
void handle_update_staff_role(int client_socket);
void handle_change_admin_pass(int client_socket);

#endif // SERVER_LOGIC_H

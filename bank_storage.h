/*
 * ========================================
 * bank_storage.h
 * =Description: Defines the data structures for
 * the banking system's file-based database.
 * ========================================
 */

#ifndef BANK_STORAGE_H
#define BANK_STORAGE_H

// --- Constants ---
#define ACCOUNT_DB_FILE "accounts.dat"
#define STAFF_DB_FILE "staff.dat"
#define LOAN_DB_FILE "loans.dat"
#define TRANSACTION_DB_FILE "transactions.dat"
#define FEEDBACK_DB_FILE "feedback.dat"
#define LOAN_COUNTER_FILE "loan_id.dat"
#define ADMIN_PASS_FILE "admin_auth.dat"

// --- Data Structures ---

// Represents a single customer account record
struct CustomerAccount {
    int account_id;
    char owner_name[50];
    char access_pin[20];
    double balance;
    int is_active; // 1 for active, 0 for inactive
};

// Represents a single staff member (Employee or Manager)
struct EmployeeRecord {
    int employee_id;
    char first_name[25];
    char last_name[25];
    char login_pass[20];
    int role; // 0=Manager, 1=Employee
};

// Represents a loan application
struct LoanApplication {
    int loan_id;
    int customer_account_id; // Links to CustomerAccount
    double amount;
    int status; // 0=Requested, 1=Assigned, 2=Approved, 3=Rejected
    int assigned_to_employee_id; // Links to EmployeeRecord
};

// For storing transaction history
struct Transaction {
    int account_id;
    char timestamp[30];
    char description[100]; // e.g., "DEPOSIT +500.00"
    double resulting_balance;
};

// For storing user feedback
struct FeedbackEntry {
    char feedback_text[256];
};

// For auto-incrementing loan IDs
struct IDCounter {
    int next_loan_id;
};

#endif // BANK_STORAGE_H

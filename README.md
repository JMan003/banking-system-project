# Banking Management System

This project is a client-server Banking Management System designed for the CS-513 System Software course. 
It simulates core functionalities of a bank, such as customer account management, transactions, and loan processing.

The primary focus is on handling concurrent client requests using system-level programming concepts, ensuring data consistency, and preventing race conditions.

---

## ⚙️ Core Technical Features

This system is built using low-level C and POSIX APIs.

### 1. Client-Server Architecture
- Implemented using Socket Programming.
- The server is multi-process (uses fork() for each client) and handles multiple clients concurrently.

### 2. System Call-Based I/O
- All database operations (for accounts, staff, loans, etc.) are performed using low-level system calls:
  `open`, `read`, `write`, and `lseek`.
- No use of standard library I/O (e.g., `fopen`, `fread`, `fwrite`).

### 3. Concurrency & Synchronization
- **File Locking:**  
  Uses `fcntl` advisory locks (shared read `F_RDLCK`, exclusive write `F_WRLCK`) to protect individual records.  
  Prevents race conditions and ensures ACID compliance.

- **Session Management:**  
  Implements POSIX named semaphores (`sem_open`, `sem_trywait`, `sem_post`) to enforce “one session per user”.  
  Prevents multiple logins for the same user simultaneously.

---

## 👥 Modules & Functionality

The system supports four user roles, each with specific permissions:

### Administrator
- Add new bank employees or managers.
- Modify customer or employee details.
- Change employee roles (e.g., promote to manager).
- Change their own admin password.

### Manager
- Activate or deactivate customer accounts.
- Assign loan applications to employees for processing.
- Review all customer feedback.

### Bank Employee
- Add new customer accounts.
- Modify customer account details.
- Process (approve/reject) assigned loan applications.
- View any customer's transaction history.

### Customer
- View account balance.
- Deposit and withdraw money.
- Transfer funds between customer accounts.
- Apply for a loan.
- View personal transaction history.
- Change password/PIN.
- Submit feedback.

---

## 🛠️ How to Compile

The project generates two executables: **server** and **client**.

### Compile Server
```bash
gcc server.c server_logic.c utils.c -o server -pthread
```

### Compile Client
```bash
gcc client.c -o client
```

---

## 🚀 How to Run

### 1. Start the Server
```bash
./server
```
Output: `Server listening on port 8080...`

### 2. Start the Client (in another terminal)
```bash
./client
```
The client connects to the server and shows the main login menu.

---

## 🏁 First-Time Setup (Important)

If running for the first time (no `.dat` files exist):

1. Run `./server`
2. Run `./client` and log in as **Admin**
   - Password: `root123`
   - This creates `admin_auth.dat`.
3. As Admin:
   - Add a Manager (ID: 1, Role: 0)
   - Add an Employee (ID: 2, Role: 1)
   - This creates `staff.dat`.
4. Log out and log in as the Employee (ID: 2)
   - Add a Customer (e.g., Account ID: 101)
   - This creates `accounts.dat`.
5. You can now log in as:
   - Customer (ID 101)
   - Manager (ID 1)
   - Admin

---

## 📁 File Structure

### Header Files (.h)
- `bank_storage.h`: Defines all structs for the database records.
- `utils.h`: Utility function prototypes (socket I/O, session handling, record operations).
- `server_logic.h`: Function prototypes for all business logic actions.

### Server Source Files (.c)
- `server.c`: Handles socket setup, bind, listen, and fork for new clients.
- `server_logic.c`: Implements user actions (deposit, staff creation, etc.).
- `utils.c`: Helper functions (send_response, create_session_lock, record offset finders).

### Client Source File (.c)
- `client.c`: Client application; connects to server, handles input/output, and displays menus.

---

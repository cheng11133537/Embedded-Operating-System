// client.c
// Example usage:
//   ./client 127.0.0.1 9000 U1
//   ./client 127.0.0.1 9000 U2


// Supported commands (entered by the user):
//   reserve [id]  -> RESERVE <id>
//   checkin [id]  -> CHECKIN <id>
//   checkout [id] -> CHECKOUT <id>
//   cancel  [id]  -> CANCEL <id>
//   query [id]    -> QUERY_EQUIP <id>
//   mystatus      -> QUERY_USER (status across all equipment)
//   quit          -> gracefully exit (server will free resources)


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#define LINE_BUF_SIZE 1024
#define EQUIP_COUNT 2


// Main menu
typedef enum {
    UI_MAIN_MENU = 0,
    UI_RESERVE_EQUIP,
    UI_CANCEL_EQUIP,
    UI_QUERY_EQUIP,
    UI_WAIT_MAIN_ACK
} UiState;


//Send a full line to the server, ensuring newline and complete transmission
static int send_line(int fd, const char *line)
{
    char buf[LINE_BUF_SIZE];
    size_t len = strlen(line);
    // Ensure the message fits within buffer size limits
    if (len + 1 >= sizeof(buf)) {
        fprintf(stderr, "line too long\n");
        return -1;
    }
    // Ensure that a newline exists (server is line-oriented)
    if (len > 0 && line[len-1] == '\n') {
        snprintf(buf, sizeof(buf), "%s", line);
    } else {
        snprintf(buf, sizeof(buf), "%s\n", line);
    }
    // Make sure entire message is sent (handle partial send behavior of TCP)
    size_t total = 0;
    size_t buflen = strlen(buf);
    while (total < buflen) {
        ssize_t n = send(fd, buf + total, buflen - total, 0);
        if (n < 0) {
            // Retry if interrupted by signal
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}
// Receive one full line from the server
static int recv_line(int fd, char *buf, size_t size)
{
    size_t pos = 0;
    while (pos < size - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue; // Retry on signal interruption
            perror("recv");
            return -1;
        }
        if (n == 0) {
            // Server closed connection
            if (pos == 0) return 0;
            break;
        }
        if (c == '\n')
            break; // End of one command line
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}


int main(int argc, char *argv[])
{
    // Validate command-line arguments
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <user_id>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    const char *user_id = argv[3];
    // Create TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    // Setup server address structure
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(port);
    // Convert IP string to binary format
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return EXIT_FAILURE;
    }
    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }
    char buf[LINE_BUF_SIZE];
    char login_cmd[LINE_BUF_SIZE];
    snprintf(login_cmd, sizeof(login_cmd), "LOGIN %s", user_id);
    if (send_line(sockfd, login_cmd) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }
    int n = recv_line(sockfd, buf, sizeof(buf));
    if (n <= 0) {
        fprintf(stderr, "server closed early\n");
        close(sockfd);
        return EXIT_FAILURE;
    }
    printf("> %s\n", buf);


    // Display command help
    printf("===== Gym Reservation System (user=%s) =====\n", user_id);
    printf("Main menu:\n");
    printf("1. Reserve equipment\n");
    printf("2. Cancel reservation / leave queue\n");
    printf("3. Query single equipment status\n");
    printf("4. Query my status\n");
    printf("5. Quit\n\n");
    printf("Please enter your choice (1-5): ");
    fflush(stdout);


    UiState ui_state = UI_MAIN_MENU;
    int running = 1;
    int is_banned = 0;
    int ban_remain = 0; 

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);        // stdin
        FD_SET(sockfd, &rfds);   // socket
        int maxfd = sockfd;
        int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue; // retry if interrupted
            perror("select");
            break;
        }
        // Incoming message from server
        if (FD_ISSET(sockfd, &rfds)) {
            int r = recv_line(sockfd, buf, sizeof(buf));
            if (r <= 0) {
                printf("Server closed.\n");
                break;
            }
            printf("\n> %s\n", buf);
             // detect BANNED / NORMAL from server messages
            int ban_sec = 0;
            if (sscanf(buf, "USER_STATUS BANNED %d", &ban_sec) == 1 ||
                sscanf(buf, "RESERVE_FAIL %*d BANNED %d", &ban_sec) == 1 ||
                sscanf(buf, "INFO %*d BANNED %d", &ban_sec) == 1) {
                is_banned  = 1;
                ban_remain = ban_sec;
            } else if (strncmp(buf, "USER_STATUS NORMAL", 18) == 0) {
                is_banned  = 0;
                ban_remain = 0;
            }
            // After updating state, tell user to press Enter to return to the main menu
            if (ui_state != UI_WAIT_MAIN_ACK) {
                printf("\nState updated. Press Enter to return to the main menu.\n");
                fflush(stdout);
                ui_state = UI_WAIT_MAIN_ACK;
            }
        }
        // User input from keyboard
        if (FD_ISSET(0, &rfds)) {
            char line[LINE_BUF_SIZE];
            if (!fgets(line, sizeof(line), stdin)) {
                printf("EOF on stdin.\n");
                break;
            }
            // Remove newline
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }
           
            // Any key to return to the main menu (after state is updated)
            if (ui_state == UI_WAIT_MAIN_ACK) {
                printf("\n===== Gym Reservation Client (user=%s) =====\n", user_id);
                printf("Main menu:\n");
                printf("1. Reserve equipment\n");
                printf("2. Cancel reservation / leave queue\n");
                printf("3. Query single equipment status\n");
                printf("4. Query my status\n");
                printf("5. Quit\n\n");
                printf("Please enter your choice (1-5): ");
                fflush(stdout);
                ui_state = UI_MAIN_MENU;
                continue;
            }


            if (line[0] == '\0') {
                if (ui_state == UI_MAIN_MENU) {
                    printf("Please enter your choice (1-5): ");
                    fflush(stdout);
                }
                continue;
            }            

            // handle "main" to jump to main menu from any state
            if (strcmp(line, "main") == 0) {
                printf("\n===== Gym Reservation Client (user=%s) =====\n", user_id);
                printf("Main menu:\n");
                printf("1. Reserve equipment\n");
                printf("2. Cancel reservation / leave queue\n");
                printf("3. Query single equipment status\n");
                printf("4. Query my status\n");
                printf("5. Quit\n\n");
                printf("Please enter your choice (1-5): ");
                fflush(stdout);
                ui_state = UI_MAIN_MENU;
                continue;
            }

            // previous step handler
            if (strcmp(line, "back") == 0) {
                if (ui_state == UI_MAIN_MENU) {
                    printf("Already at main menu.\n");
                    printf("Please enter your choice (1-5): ");
                    fflush(stdout);
                } else {
                    ui_state = UI_MAIN_MENU;
                    printf("\nBack to main menu.\n");
                    printf("Please enter your choice (1-5): ");
                    fflush(stdout);
                }
                continue;
            }


            if (ui_state == UI_MAIN_MENU) {
                int choice = atoi(line);
                
                // If user is banned, block options 1 and 2 directly
                if (is_banned && (choice == 1 || choice == 2)) {
                    printf("\n*** You are currently banned. ***\n");
                    printf("Ban remaining time: %d seconds.\n", ban_remain);
                    printf("You cannot reserve or cancel equipment during the ban.\n");
                    printf("Please choose another option (3-5), or wait until the ban expires.\n");
                    printf("Please enter your choice (1-5): ");
                    fflush(stdout);
                    continue;
                }

                switch (choice) {
                case 1:
                    printf("\nEnter equipment ID to reserve (0~%d): ", EQUIP_COUNT - 1);
                    fflush(stdout);
                    ui_state = UI_RESERVE_EQUIP;
                    break;
                case 2:
                    printf("\nEnter equipment ID to cancel / leave queue (0~%d): ", EQUIP_COUNT - 1);
                    fflush(stdout);
                    ui_state = UI_CANCEL_EQUIP;
                    break;
                case 3:
                    printf("\nEnter equipment ID to query (0~%d): ", EQUIP_COUNT - 1);
                    fflush(stdout);
                    ui_state = UI_QUERY_EQUIP;
                    break;
                case 4:
                    send_line(sockfd, "QUERY_USER");
                    printf("Sent QUERY_USER to server.\n");
                    break;
                case 5:
                    printf("Bye.\n");
                    running = 0;
                    break;
                default:
                    printf("Invalid choice, please enter 1-5.\n");
                    printf("Please enter your choice (1-5): ");
                    fflush(stdout);
                    break;


                }
            } else {
                // second step of entering device ID
                int is_digit_only = 1;
                for (size_t i = 0; i < len; i++) {
                    if (line[i] < '0' || line[i] > '9') {
                        is_digit_only = 0;
                        break;
                    }
                }
                if (!is_digit_only) {
                    printf("Invalid equipment ID, please enter an integer between 0 and %d: ",
                           EQUIP_COUNT - 1);
                    fflush(stdout);
                    // ui_state won’t change, let user input again
                    continue;
                }

                int equip_id = atoi(line);
                if (equip_id < 0 || equip_id >= EQUIP_COUNT) {
                    printf("Equipment ID must be between 0 and %d, please enter again: ",
                           EQUIP_COUNT - 1);
                    fflush(stdout);
                    // ui_state won’t change, let user input again
                    continue;
                }

                if (ui_state == UI_RESERVE_EQUIP) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "RESERVE %d", equip_id);
                    send_line(sockfd, msg);
                } else if (ui_state == UI_CANCEL_EQUIP) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "CANCEL %d", equip_id);
                    send_line(sockfd, msg);
                } else if (ui_state == UI_QUERY_EQUIP) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "QUERY_EQUIP %d", equip_id);
                    send_line(sockfd, msg);
                }

                // back to main menu
                ui_state = UI_MAIN_MENU;
            }
        }
    }
    // Cleanup
    close(sockfd);
    return 0;
}








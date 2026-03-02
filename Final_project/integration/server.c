// server.c
// Example: ./server 9000 127.0.0.1 10000 10001

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include "protocol_common.h"
#include "log_common.h"

#define EQUIP_COUNT     2    // number of equipment devices
#define MAX_CLIENTS     4    // max concurrent sockets (users + drivers)
#define LINE_BUF_SIZE   1024 // max line length for protocol
#define USER_ID_LEN     32   // max user id length
#define MAX_QUEUE       3    // max queued users per equipment (plus 1 waiting = 4)

#define CHECKIN_WINDOW  60          // seconds to swipe/checkin after turn granted
#define USAGE_TIME      20           // seconds of allowed equipment use
#define BAN_TIME        20          // ban seconds for missing check-in
#define CONTINUE_DECISION_WINDOW 5   // seconds to accept continue/decline response
#define QUICK_CHECKIN_WINDOW 5       // quick 5-second window when user already using another equip
// Client role: user or driver
typedef enum {
    ROLE_NONE = 0,
    ROLE_USER,
    ROLE_DRIVER
} ClientRole;
// Equipment states
typedef enum {
    EQUIP_IDLE = 0,
    EQUIP_WAITING_CHECKIN,
    EQUIP_IN_USE,
    EQUIP_WAITING_CONTINUE
} EquipState;
typedef enum {
    DRIVER_EVENT_NONE = 0,
    DRIVER_EVENT_MISSED,
    DRIVER_EVENT_FINISHED,
    DRIVER_EVENT_CHECKEDIN,
} DriverEventType;
typedef enum {
    COUNTDOWN_PHASE_CHECKIN = 0,
    COUNTDOWN_PHASE_USAGE
} CountdownPhase;
// State for each connected client socket
typedef struct {
    int        fd;
    ClientRole role;
    char       user_id[USER_ID_LEN];   
    time_t     ban_end;                
    char inbuf[LINE_BUF_SIZE];
    int  inbuf_len;             //Buffer for receiving a single line of instructions
} Client;
// Runtime state for each equipment/driver connection
typedef struct {
    EquipState state;
    int        id;
    int        sock;
    char  wait_user[USER_ID_LEN];   // Waiting for card swipe
    time_t wait_deadline;           // Deadline for card swipe
    char  use_user[USER_ID_LEN];    // Currently using
    time_t use_end_time;            // Usage end time
    char queue[MAX_QUEUE][USER_ID_LEN];
    int  queue_len;                 // 0~MAX_QUEUE
    DriverEventType pending_event;
    char pending_event_user[USER_ID_LEN];
    char continue_user[USER_ID_LEN];
    time_t continue_deadline;
    int  wait_quick_checkin;
} Equipment;

static int handle_driver_event(Equipment *eq, const char *line);
static int process_driver_event_if_pending(Equipment *eq);
static void release_usage_and_promote(Equipment *eq, Client *clients, int max_clients, int prompt_continue);

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_lock;      // protects g_eqs and g_clients
static Equipment g_eqs[EQUIP_COUNT];
static Client g_clients[MAX_CLIENTS];

// ====== Equipment driver connection helpers ======
static int equip_connect(Equipment *eq, const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(equip)");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton(equip)");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect(equip)");
        close(fd);
        return -1;
    }
    LOG_INFO("EquipDriver", "Connected to equipment #%d %s:%d", eq->id, ip, port);
    eq->sock = fd;
    return 0;
}
// Send a full line to driver socket (low-level helper)
static int equip_send_line(const Equipment *eq, const char *line)
{
    if (!eq || eq->sock < 0) 
    {
        return -1;
    }
    size_t len = strlen(line);
    size_t sent = 0;
    //send instructions
    while (sent < len) {
        ssize_t n = send(eq->sock, line + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send(equip)");
            return -1;
        }
        if (n == 0) 
        {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}
// Receive one line from driver socket (low-level helper)
static int equip_recv_line(Equipment *eq, char *buf, size_t size)
{
    if (!eq || eq->sock < 0) 
    {
        return -1;
    }
    while (1) {
        size_t pos = 0;
        //receive instructions
        while (pos < size - 1) {
            char c;
            ssize_t n = recv(eq->sock, &c, 1, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv(equip)");
                return -1;
            }
            if (n == 0) {
                if (pos == 0) return 0;
                break;
            }
            if (c == '\n') break;
            buf[pos++] = c;
        }
        buf[pos] = '\0';
        if (handle_driver_event(eq, buf)) {
            continue;
        }
        return (int)pos;
    }
}
// startCountdown <seconds> <user_id>
// High-level helper: tell driver to start countdown, wait for ACK
static int equip_start_countdown(Equipment *eq, int seconds, const char *user_id, CountdownPhase phase)
{
    if (!eq || eq->sock < 0) 
    {
        return -1;
    }
    char cmd[64];
    const char *phase_str = (phase == COUNTDOWN_PHASE_CHECKIN) ? "CHECKIN" : "USAGE";
    if (user_id && *user_id) {
        snprintf(cmd, sizeof(cmd), "STARTCOUNTDOWN %d %s %s\n", seconds, user_id, phase_str);
    } else {
        snprintf(cmd, sizeof(cmd), "STARTCOUNTDOWN %d %s\n", seconds, phase_str);
    }
    //Send the string instruction to the driver
    if (equip_send_line(eq, cmd) < 0) 
    {
        return -1;
    }
    char resp[64];
    int n = equip_recv_line(eq, resp, sizeof(resp));
    if (n <= 0) 
    {
        return -1;
    }
    process_driver_event_if_pending(eq);
    int code = 0;
    //print info in the server terminal
    if (sscanf(resp, "ACK %d", &code) == 1 && code == 1) {
        LOG_INFO("EquipDriver", "EQUIP #%d STARTCOUNTDOWN (%d) OK", eq->id, seconds);
        return 0;
    }
    LOG_WARN("EquipDriver", "EQUIP #%d STARTCOUNTDOWN (%d) FAIL: %s", eq->id, seconds, resp);
    return -1;
}
// stopCountdown (high-level helper): ask driver to stop timer, expect ACK
static int equip_stop_countdown(Equipment *eq)
{
    if (!eq || eq->sock < 0) 
    {
        return -1;
    }
    //Send the string instruction to the driver
    if (equip_send_line(eq, "STOPCOUNTDOWN\n") < 0) 
    {
        return -1;
    }
    char resp[64];
    int n = equip_recv_line(eq, resp, sizeof(resp));
    if (n <= 0) 
    {
        return -1;
    }
    process_driver_event_if_pending(eq);
    int code = 0;
    //print info in the server terminal
    if (sscanf(resp, "ACK %d", &code) == 1 && code == 1) {
        LOG_INFO("EquipDriver", "EQUIP #%d STOPCOUNTDOWN OK", eq->id);
        return 0;
    }
    LOG_WARN("EquipDriver", "EQUIP #%d STOPCOUNTDOWN FAIL: %s", eq->id, resp);
    return -1;
}
// getStatus: ask driver for current user + remaining seconds
static int equip_get_status(Equipment *eq, char *user_buf, size_t user_buf_sz, int *time_remain)
{
    if (!eq || eq->sock < 0) 
    {
        return -1;
    }
    if (equip_send_line(eq, "GETSTATUS\n") < 0) 
    {
        return -1;
    }
    char resp[64];
    int n = equip_recv_line(eq, resp, sizeof(resp));
    if (n <= 0) 
    {
        return -1;
    }
    process_driver_event_if_pending(eq);
    char uid[32];
    int t = 0;
    if (sscanf(resp, "%31s %d", uid, &t) == 2) {
        snprintf(user_buf, user_buf_sz, "%s", uid);
        if (time_remain) *time_remain = t;
        return 0;
    }
    return -1;
}
// ====== Gym server internal logic ======
static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

// Trim trailing CR/LF from a line in-place
static void trim_newline(char *s)
{
    if (!s) 
    {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[--len] = '\0';
    }
}
static int send_all(int fd, const char *msg)
{
    size_t len = strlen(msg);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, msg + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) 
        {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}
// Print a formatted event log on server stdout (not broadcast to clients)
static void broadcast_log(Client *clients, int max_clients, const char *fmt, ...)
{
    (void)clients;
    (void)max_clients;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG_INFO("Server", "%s", msg);
    fflush(stdout);
}

static int create_listen_socket(uint16_t port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 8) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}
// Find the user with the specified user_id among all connected clients
static int find_client_by_user(Client *clients, int max, const char *user_id)
{
    for (int i = 0; i < max; ++i) {
        if (clients[i].fd > 0 &&
            clients[i].role == ROLE_USER &&
            strcmp(clients[i].user_id, user_id) == 0) {
            return i;
        }
    }
    return -1;
}
// Send a message to a specified user 
static void send_to_user(Client *clients, int max, const char *user_id, const char *msg)
{
    int idx = find_client_by_user(clients, max, user_id);
    if (idx >= 0) {
        send_all(clients[idx].fd, msg);
    }
}
// push_back user_id into queue 
static int enqueue_user(Equipment *eq, const char *user_id)
{
    if (eq->queue_len >= MAX_QUEUE)
    {
        return -1;
    }
    snprintf(eq->queue[eq->queue_len], USER_ID_LEN, "%s", user_id);
    eq->queue[eq->queue_len][USER_ID_LEN-1] = '\0';
    eq->queue_len++;
    return eq->queue_len; 
}
// delete user_id from queue
static void remove_from_queue(Equipment *eq, const char *user_id)
{
    for (int i = 0; i < eq->queue_len; ++i) {
        if (strcmp(eq->queue[i], user_id) == 0) {
            for (int j = i + 1; j < eq->queue_len; ++j) {
                snprintf(eq->queue[j-1], USER_ID_LEN, "%s", eq->queue[j]);
                eq->queue[j-1][USER_ID_LEN-1] = '\0';
            }
            eq->queue_len--;
            break;
        }
    }
}
// FIFO
static int dequeue_user(Equipment *eq, char *out_user)
{
    if (eq->queue_len == 0)
    {
        return 0;
    }
    snprintf(out_user, USER_ID_LEN, "%s", eq->queue[0]);
    out_user[USER_ID_LEN-1] = '\0';
    for (int i = 1; i < eq->queue_len; ++i) {
        snprintf(eq->queue[i-1], USER_ID_LEN, "%s", eq->queue[i]);
        eq->queue[i-1][USER_ID_LEN-1] = '\0';
    }
    eq->queue_len--;
    return 1;
}
//To avoid the same person making multiple appointments or entering the same equipment multiple times
static int user_in_equipment(const Equipment *eq, const char *user_id)
{
    if (eq->state == EQUIP_WAITING_CHECKIN &&strcmp(eq->wait_user, user_id) == 0) return 1;
    if (eq->state == EQUIP_IN_USE &&strcmp(eq->use_user, user_id) == 0) return 1;
    for (int i = 0; i < eq->queue_len; ++i) {
        if (strcmp(eq->queue[i], user_id) == 0) return 1;
    }
    return 0;
}
//Calculate how many devices a user is currently using simultaneously
static int user_active_count(Equipment *eqs, int eq_count, const char *user_id)
{
    int count = 0;
    for (int i = 0; i < eq_count; ++i) {
        if (user_in_equipment(&eqs[i], user_id)) {
            count++;
        }
    }
    return count;
}

static int user_currently_using(const Equipment *eqs, int eq_count, const char *user_id)
{
    if (!user_id || user_id[0] == '\0') {
        return 0;
    }
    for (int i = 0; i < eq_count; ++i) {
        if (eqs[i].state == EQUIP_IN_USE &&
            strcmp(eqs[i].use_user, user_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static Equipment* get_equipment_by_id(Equipment *eqs, int eq_count, int equip_id)
{
    if (equip_id < 0 || equip_id >= eq_count) 
    {
        return NULL;
    }
    return &eqs[equip_id];
}
// Give the usage right to the next user in the queue (or set to IDLE)
static int give_turn_to_next(Equipment *eq, Client *clients, int max_clients)
{
    time_t now = time(NULL);
    (void)now;
    char next_user[USER_ID_LEN];
    if (dequeue_user(eq, next_user)) {
        int quick = user_currently_using(g_eqs, EQUIP_COUNT, next_user);
        int wait_seconds = quick ? QUICK_CHECKIN_WINDOW : CHECKIN_WINDOW;
        eq->state = EQUIP_WAITING_CHECKIN;
        snprintf(eq->wait_user, USER_ID_LEN, "%s", next_user);
        eq->wait_user[USER_ID_LEN-1] = '\0';
        eq->wait_deadline = time(NULL) + wait_seconds;
        eq->wait_quick_checkin = quick;
        equip_start_countdown(eq, wait_seconds, next_user, COUNTDOWN_PHASE_CHECKIN);
        char msg[128];
        snprintf(msg, sizeof(msg), "INFO %d YOUR_TURN 1 %d\n", eq->id, wait_seconds);
        send_to_user(clients, max_clients, next_user, msg);
        broadcast_log(clients, max_clients, "TURN E%d %s %d", eq->id, next_user, wait_seconds);
        return 1;
    }
    eq->state = EQUIP_IDLE;
    eq->wait_quick_checkin = 0;
    eq->wait_user[0] = '\0';
    eq->wait_deadline = 0;
    broadcast_log(clients, max_clients, "EQUIP_IDLE E%d", eq->id);
    return 0;
}

static void clear_continue_prompt(Equipment *eq)
{
    if (!eq) {
        return;
    }
    eq->continue_user[0] = '\0';
    eq->continue_deadline = 0;
}

static void cancel_duplicate_waiting_checkin(const char *user_id, Equipment *active_eq)
{
    if (!user_id || user_id[0] == '\0') {
        return;
    }
    for (int i = 0; i < EQUIP_COUNT; ++i) {
        Equipment *eq = &g_eqs[i];
        if (eq == active_eq) {
            continue;
        }
        if (eq->state == EQUIP_WAITING_CHECKIN && strcmp(eq->wait_user, user_id) == 0) {
            broadcast_log(g_clients, MAX_CLIENTS,
                          "AUTO_CANCEL_WAIT E%d %s", eq->id, user_id);
            equip_stop_countdown(eq);
            eq->state = EQUIP_IDLE;
            eq->wait_user[0] = '\0';
            eq->wait_deadline = 0;
            give_turn_to_next(eq, g_clients, MAX_CLIENTS);
        }
    }
}

static void prompt_user_continue(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    clear_continue_prompt(eq);
    eq->state = EQUIP_WAITING_CONTINUE;
    snprintf(eq->continue_user, USER_ID_LEN, "%s", user_id);
    eq->continue_user[USER_ID_LEN-1] = '\0';
    eq->continue_deadline = time(NULL) + CONTINUE_DECISION_WINDOW;
    char msg[128];
    snprintf(msg, sizeof(msg), "%s %d %d\n", MSG_CONTINUE_PROMPT, eq->id, CONTINUE_DECISION_WINDOW);
    send_to_user(g_clients, MAX_CLIENTS, user_id, msg);
    broadcast_log(g_clients, MAX_CLIENTS, "PROMPT_CONTINUE E%d %s", eq->id, user_id);
}

static void start_continue_checkin(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    eq->state = EQUIP_WAITING_CHECKIN;
    snprintf(eq->wait_user, USER_ID_LEN, "%s", user_id);
    eq->wait_user[USER_ID_LEN-1] = '\0';
    eq->wait_deadline = time(NULL) + CHECKIN_WINDOW;
    clear_continue_prompt(eq);
    if (equip_start_countdown(eq, CHECKIN_WINDOW, user_id, COUNTDOWN_PHASE_CHECKIN) < 0) {
        LOG_WARN("Server", "EQUIP #%d CONTINUE start countdown failed", eq->id);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "INFO %d CONTINUE_YES %d\n", eq->id, CHECKIN_WINDOW);
    send_to_user(g_clients, MAX_CLIENTS, user_id, msg);
    broadcast_log(g_clients, MAX_CLIENTS, "CONTINUE_START E%d %s", eq->id, user_id);
}

static void decline_continue(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    clear_continue_prompt(eq);
    eq->state = EQUIP_IDLE;
    char msg[128];
    snprintf(msg, sizeof(msg), "INFO %d CONTINUE_NO\n", eq->id);
    send_to_user(g_clients, MAX_CLIENTS, user_id, msg);
    broadcast_log(g_clients, MAX_CLIENTS, "CONTINUE_NO E%d %s", eq->id, user_id);
    give_turn_to_next(eq, g_clients, MAX_CLIENTS);
}

static void ban_user_for_missing(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0')
        return;
    time_t now = time(NULL);
    int idx = find_client_by_user(g_clients, MAX_CLIENTS, user_id);
    if (idx >= 0) {
        g_clients[idx].ban_end = now + BAN_TIME;
        char msg[128];
        snprintf(msg, sizeof(msg), "INFO %d BANNED %d\n", eq->id, BAN_TIME);
        send_all(g_clients[idx].fd, msg);
    }
    broadcast_log(g_clients, MAX_CLIENTS, "BAN E%d %s %d", eq->id, user_id, BAN_TIME);
    eq->state = EQUIP_IDLE;
    eq->wait_user[0] = '\0';
    eq->wait_deadline = 0;
    eq->wait_quick_checkin = 0;
    equip_stop_countdown(eq);
    clear_continue_prompt(eq);
    give_turn_to_next(eq, g_clients, MAX_CLIENTS);
}

static void notify_user_requeue(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "INFO %d REQUEUE_LAST\n", eq->id);
    send_to_user(g_clients, MAX_CLIENTS, user_id, msg);
}

static void requeue_user_for_late_arrival(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    if (enqueue_user(eq, user_id) < 0) {
        broadcast_log(g_clients, MAX_CLIENTS, "MOVE_BACK_FULL E%d %s", eq->id, user_id);
        return;
    }
    broadcast_log(g_clients, MAX_CLIENTS, "MOVE_BACK E%d %s", eq->id, user_id);
    notify_user_requeue(eq, user_id);
}

static void handle_quick_checkin_timeout(Equipment *eq, Client *clients, int max_clients, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0') {
        return;
    }
    eq->wait_quick_checkin = 0;
    eq->wait_user[0] = '\0';
    eq->wait_deadline = 0;
    equip_stop_countdown(eq);
    requeue_user_for_late_arrival(eq, user_id);
    give_turn_to_next(eq, clients, max_clients);
}

static void mark_user_checked_in(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0')
        return;
    cancel_duplicate_waiting_checkin(user_id, eq);
    time_t now = time(NULL);
    eq->state = EQUIP_IN_USE;
    snprintf(eq->use_user, USER_ID_LEN, "%s", user_id);
    eq->use_user[USER_ID_LEN-1] = '\0';
    eq->use_end_time = now + USAGE_TIME;
    eq->wait_user[0] = '\0';
    eq->wait_deadline = 0;
    eq->wait_quick_checkin = 0;
    char msg[128];
    snprintf(msg, sizeof(msg), "CHECKIN_OK %d START_USE %d\n", eq->id, USAGE_TIME);
    send_to_user(g_clients, MAX_CLIENTS, user_id, msg);
    broadcast_log(g_clients, MAX_CLIENTS, "START E%d %s %d", eq->id, user_id, USAGE_TIME);
    if (equip_start_countdown(eq, USAGE_TIME, user_id, COUNTDOWN_PHASE_USAGE) < 0) {
        LOG_WARN("Server", "EQUIP #%d failed to restart usage timer for %s", eq->id, user_id);
    }
}

static void handle_user_finished_event(Equipment *eq, const char *user_id)
{
    if (!eq || !user_id || user_id[0] == '\0')
        return;
    if (eq->state == EQUIP_IN_USE && strcmp(eq->use_user, user_id) == 0) {
        release_usage_and_promote(eq, g_clients, MAX_CLIENTS, 0);
    }
}

static int handle_driver_event(Equipment *eq, const char *line)
{
    if (!eq || !line || line[0] == '\0') {
        return 0;
    }
    char cmd[32];
    char user_id[USER_ID_LEN];
    int parts = sscanf(line, "%31s %31s", cmd, user_id);
    if (parts < 2) {
        return 0;
    }
    if (strcmp(cmd, EVT_USER_MISSED) == 0) {
        eq->pending_event = DRIVER_EVENT_MISSED;
        snprintf(eq->pending_event_user, USER_ID_LEN, "%s", user_id);
        eq->pending_event_user[USER_ID_LEN-1] = '\0';
        return 1;
    }
    if (strcmp(cmd, EVT_USER_FINISHED) == 0) {
        eq->pending_event = DRIVER_EVENT_FINISHED;
        snprintf(eq->pending_event_user, USER_ID_LEN, "%s", user_id);
        eq->pending_event_user[USER_ID_LEN-1] = '\0';
        return 1;
    }
    if (strcmp(cmd, EVT_USER_CHECKEDIN) == 0) {
        eq->pending_event = DRIVER_EVENT_CHECKEDIN;
        snprintf(eq->pending_event_user, USER_ID_LEN, "%s", user_id);
        eq->pending_event_user[USER_ID_LEN-1] = '\0';
        return 1;
    }
    return 0;
}

static int process_driver_event_if_pending(Equipment *eq)
{
    if (!eq || eq->pending_event == DRIVER_EVENT_NONE)
    {
        return 0;
    }
    DriverEventType event = eq->pending_event;
    char pending_user[USER_ID_LEN];
    snprintf(pending_user, sizeof(pending_user), "%s", eq->pending_event_user);
    pending_user[USER_ID_LEN-1] = '\0';

    eq->pending_event = DRIVER_EVENT_NONE;
    eq->pending_event_user[0] = '\0';

    switch (event) {
    case DRIVER_EVENT_MISSED:
        if (eq->wait_quick_checkin) {
            handle_quick_checkin_timeout(eq, g_clients, MAX_CLIENTS, pending_user);
        } else {
            ban_user_for_missing(eq, pending_user);
        }
        break;
    case DRIVER_EVENT_FINISHED:
        handle_user_finished_event(eq, pending_user);
        break;
    case DRIVER_EVENT_CHECKEDIN:
        mark_user_checked_in(eq, pending_user);
        break;
    default:
        break;
    }
    return 1;
}
// User leaves (CHECKOUT / disconnect / time-up): stop timer, promote next
static void release_usage_and_promote(Equipment *eq, Client *clients, int max_clients, int prompt_continue)
{
    if (eq->state != EQUIP_IN_USE)
    {
        return;
    }
    char finished_user[USER_ID_LEN];
    snprintf(finished_user, USER_ID_LEN, "%s", eq->use_user);
    finished_user[USER_ID_LEN-1] = '\0';
    equip_stop_countdown(eq);
    eq->use_user[0] = '\0';
    eq->use_end_time = 0;
    eq->state = EQUIP_IDLE;
    eq->wait_quick_checkin = 0;
    broadcast_log(clients, max_clients,"FINISH E%d %s", eq->id, finished_user);
    if (!give_turn_to_next(eq, clients, max_clients)) {
        if (prompt_continue) {
            prompt_user_continue(eq, finished_user);
        }
    }
}
// Periodic device state update: handle checkin timeout / usage timeout
static void update_equipment_state(Equipment *eq, Client *clients, int max_clients)
{
    time_t now = time(NULL);
    if (process_driver_event_if_pending(eq)) {
        return;
    }
    if (eq->state == EQUIP_WAITING_CONTINUE) {
        if (now >= eq->continue_deadline && eq->continue_user[0] != '\0') {
            decline_continue(eq, eq->continue_user);
        }
        return;
    }
    if (eq->state == EQUIP_WAITING_CHECKIN) {
        char dev_user[USER_ID_LEN];
        int dev_remain = 0;
        if (equip_get_status(eq, dev_user, sizeof(dev_user), &dev_remain) == 0) {
            if (process_driver_event_if_pending(eq)) {
                return;
            }
            if (strcmp(dev_user, "NONE") != 0 &&
                eq->wait_user[0] != '\0' &&
                strcmp(dev_user, eq->wait_user) == 0) {
                mark_user_checked_in(eq, eq->wait_user);
                return;
            }
        }
        if (now > eq->wait_deadline && eq->wait_user[0] != '\0') {
            if (eq->wait_quick_checkin) {
                handle_quick_checkin_timeout(eq, clients, max_clients, eq->wait_user);
            } else {
                ban_user_for_missing(eq, eq->wait_user);
            }
        }
    }
    if (eq->state == EQUIP_IN_USE) {
        char dev_user[USER_ID_LEN];
        int dev_remain = 0;
        if (equip_get_status(eq, dev_user, sizeof(dev_user), &dev_remain) == 0) {
            if (process_driver_event_if_pending(eq)) {
                return;
            }
            if (strcmp(dev_user, "NONE") == 0) {
                release_usage_and_promote(eq, clients, max_clients, 0);
                return;
            }
            if (strcmp(dev_user, eq->use_user) == 0 && dev_remain >= 0) {
                eq->use_end_time = now + dev_remain;
            }
        }
        if (now >= eq->use_end_time) {
            //user time up->end usage + next user
            broadcast_log(clients, max_clients,"TIMEUP E%d %s",eq->id, eq->use_user);
            release_usage_and_promote(eq, clients, max_clients, 1);
        }
    }
}
// Handle user commands (per line from client)
static void handle_user_command(Client *cli, Client *clients, int max_clients, Equipment *eqs, int eq_count, const char *line)
{
    time_t now = time(NULL);
    char buf[LINE_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s", line);
    buf[sizeof(buf)-1] = '\0';
    char *cmd = strtok(buf, " ");
    if (!cmd) 
    {
        return;
    }
    // RESERVE <equip_id>
    if (strcmp(cmd, "RESERVE") == 0) {
        char *equip_str = strtok(NULL, " ");
        if (!equip_str) 
        {
            return;
        }
        int equip_id = atoi(equip_str);
        Equipment *eq = get_equipment_by_id(eqs, eq_count, equip_id);
        if (!eq) {
            send_all(cli->fd, "RESERVE_FAIL INVALID_EQUIP\n");
            return;
        }
        if (cli->ban_end > now) {
            int remain = (int)(cli->ban_end - now);
            if (remain < 0) remain = 0;
            char msg[128];
            snprintf(msg, sizeof(msg), "RESERVE_FAIL %d BANNED %d\n", eq->id, remain);
            send_all(cli->fd, msg);
            return;
        }
        if (user_in_equipment(eq, cli->user_id)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "RESERVE_FAIL %d DUPLICATE\n", eq->id);
            send_all(cli->fd, msg);
            return;
        }
        if (user_active_count(eqs, eq_count, cli->user_id) >= eq_count) {
            char msg[128];
            snprintf(msg, sizeof(msg), "RESERVE_FAIL %d LIMIT %d\n", eq->id, eq_count);
            send_all(cli->fd, msg);
            return;
        }
        if (eq->state == EQUIP_IDLE) {
            eq->state = EQUIP_WAITING_CHECKIN;
            snprintf(eq->wait_user, USER_ID_LEN, "%s", cli->user_id);
            eq->wait_user[USER_ID_LEN-1] = '\0';
            eq->wait_deadline = now + CHECKIN_WINDOW;
            eq->queue_len = 0;
            equip_start_countdown(eq, CHECKIN_WINDOW, cli->user_id, COUNTDOWN_PHASE_CHECKIN);
            char msg[128];
            snprintf(msg, sizeof(msg), "RESERVE_OK %d FIRST %d\n", eq->id, CHECKIN_WINDOW);
            send_all(cli->fd, msg);
            broadcast_log(clients, max_clients,"RESERVE E%d %s FIRST %d",eq->id, cli->user_id, CHECKIN_WINDOW);
        } else {
            int ret = enqueue_user(eq, cli->user_id);
            if (ret < 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "RESERVE_FAIL %d FULL\n", eq->id);
                send_all(cli->fd, msg);
            } else {
                int priority = ret;
                char msg[128];
                snprintf(msg, sizeof(msg), "RESERVE_OK %d QUEUE %d\n", eq->id, priority);
                send_all(cli->fd, msg);

                broadcast_log(clients, max_clients,"RESERVE E%d %s QUEUE %d",eq->id, cli->user_id, priority);
                if (eq->state == EQUIP_WAITING_CONTINUE && eq->continue_user[0] != '\0') {
                    char cont_user[USER_ID_LEN];
                    snprintf(cont_user, USER_ID_LEN, "%s", eq->continue_user);
                    cont_user[USER_ID_LEN-1] = '\0';
                    decline_continue(eq, cont_user);
                }
            }
        }
        return;
    }
    // CHECKIN/CHECKOUT commands are no longer accepted; hardware handles them directly.
    if (strcmp(cmd, "CHECKIN") == 0) {
        send_all(cli->fd, "CHECKIN_FAIL NOT_SUPPORTED\n");
        return;
    }
    if (strcmp(cmd, "CHECKOUT") == 0) {
        send_all(cli->fd, "CHECKOUT_FAIL NOT_SUPPORTED\n");
        return;
    }
    //CANCEL <equip_id>
    if (strcmp(cmd, "CANCEL") == 0) {
        char *equip_str = strtok(NULL, " ");
        if (!equip_str){
            return;
        }
        int equip_id = atoi(equip_str);
        Equipment *eq = get_equipment_by_id(eqs, eq_count, equip_id);
        if (!eq) {
            send_all(cli->fd, "CANCEL_FAIL INVALID_EQUIP\n");
            return;
        }
        // Cancellation from WAITING_CHECKIN
        if (eq->state == EQUIP_WAITING_CHECKIN &&
            strcmp(eq->wait_user, cli->user_id) == 0) {

            equip_stop_countdown(eq);
            eq->wait_user[0]  = '\0';
            eq->wait_deadline = 0;

            broadcast_log(clients, max_clients,
                        "CANCEL_WAIT E%d %s", eq->id, cli->user_id);

            give_turn_to_next(eq, clients, max_clients);

            char msg[128];
            snprintf(msg, sizeof(msg), "CANCEL_OK %d WAITING\n", eq->id);
            send_all(cli->fd, msg);
            return;
        }

        // Cancellation from queue
        int before = eq->queue_len;
        remove_from_queue(eq, cli->user_id);
        if (eq->queue_len < before) {
            broadcast_log(clients, max_clients,
                        "CANCEL_QUEUE E%d %s", eq->id, cli->user_id);

            char msg[128];
            snprintf(msg, sizeof(msg), "CANCEL_OK %d QUEUED\n", eq->id);
            send_all(cli->fd, msg);
            return;
        }

        // NOT_FOUND
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "CANCEL_FAIL %d NOT_FOUND\n", eq->id);
            send_all(cli->fd, msg);
        }
        return;
    }
    // QUERY_EQUIP <equip_id>
    if (strcmp(cmd, "QUERY_EQUIP") == 0) {
        char *equip_str = strtok(NULL, " ");
        if (!equip_str) 
        {
            return;
        }
        int equip_id = atoi(equip_str);
        Equipment *eq = get_equipment_by_id(eqs, eq_count, equip_id);
        if (!eq) {
            send_all(cli->fd, "EQUIP_ERROR INVALID_EQUIP\n");
            return;
        }
        char dev_user[32];
        int dev_remain = 0;
        int ok = equip_get_status(eq, dev_user, sizeof(dev_user), &dev_remain);
        if (ok < 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "EQUIP_ERROR %d\n", eq->id);
            send_all(cli->fd, msg);
            return;
        }
        if (strcmp(dev_user, "NONE") == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "EQUIP_IDLE %d\n", eq->id);
            send_all(cli->fd, msg);
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "EQUIP_INUSE %d %s %d\n", eq->id, dev_user, dev_remain);
            send_all(cli->fd, msg);
        }
        return;
    }
    // QUERY_USER
    if (strcmp(cmd, "QUERY_USER") == 0) {
        if (cli->ban_end > now) {
            int remain = (int)(cli->ban_end - now);
            if (remain < 0) 
            {
                remain = 0;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "USER_STATUS BANNED %d\n", remain);
            send_all(cli->fd, msg);
            return;
        }
        int reported = 0;
        for (int e = 0; e < eq_count; ++e) {
            Equipment *eq = &eqs[e];
            if (eq->state == EQUIP_IN_USE && strcmp(eq->use_user, cli->user_id) == 0) {
                int remain = (int)(eq->use_end_time - now);
                if (remain < 0) 
                {
                    remain = 0;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "USER_STATUS USING %d %d\n", eq->id, remain);
                send_all(cli->fd, msg);
                reported = 1;
            }
            if (eq->state == EQUIP_WAITING_CHECKIN &&
                strcmp(eq->wait_user, cli->user_id) == 0) {
                int remain = (int)(eq->wait_deadline - now);
                if (remain < 0) 
                {
                    remain = 0;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "USER_STATUS WAITING_CHECKIN %d %d\n", eq->id, remain);
                send_all(cli->fd, msg);
                reported = 1;
            }
            for (int i = 0; i < eq->queue_len; ++i) {
                if (strcmp(eq->queue[i], cli->user_id) == 0) {
                    int priority = i + 1;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "USER_STATUS QUEUED %d %d\n", eq->id, priority);
                    send_all(cli->fd, msg);
                    reported = 1;
                    break;
                }
            }
        }
        if (!reported) {
            const char *msg = "USER_STATUS NORMAL\n";
            send_all(cli->fd, msg);
        }
        return;
    }
    if (strcmp(cmd, CMD_CONTINUE_RESPONSE) == 0) {
        char *equip_str = strtok(NULL, " ");
        char *answer = strtok(NULL, " ");
        if (!equip_str || !answer) {
            send_all(cli->fd, "CONTINUE_FAIL MISSING_ARGS\n");
            return;
        }
        char *endptr = NULL;
        long equip_id = strtol(equip_str, &endptr, 10);
        if (endptr == equip_str || *endptr != '\0' || equip_id < 0) {
            send_all(cli->fd, "CONTINUE_FAIL INVALID_EQUIP\n");
            return;
        }
        if (answer[1] != '\0' || (answer[0] != '0' && answer[0] != '1')) {
            char msg[128];
            snprintf(msg, sizeof(msg), "CONTINUE_FAIL %ld INVALID_CHOICE\n", equip_id);
            send_all(cli->fd, msg);
            return;
        }
        Equipment *eq = get_equipment_by_id(eqs, eq_count, (int)equip_id);
        if (!eq || eq->state != EQUIP_WAITING_CONTINUE ||
            strcmp(eq->continue_user, cli->user_id) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "CONTINUE_FAIL %ld INVALID_STATE\n", equip_id);
            send_all(cli->fd, msg);
            return;
        }
        if (answer[0] == '1') {
            start_continue_checkin(eq, cli->user_id);
        } else {
            decline_continue(eq, cli->user_id);
        }
        return;
    }
    // // DEVSTATUS (debug)
    // if (strcmp(cmd, "DEVSTATUS") == 0) {
    //     char *equip_str = strtok(NULL, " ");
    //     int equip_id = equip_str ? atoi(equip_str) : 0;
    //     Equipment *eq = get_equipment_by_id(eqs, eq_count, equip_id);
    //     if (!eq) {
    //         send_all(cli->fd, "[DEBUG] device: INVALID_EQUIP\n");
    //         return;
    //     }

    //     char dev_user[32];
    //     int dev_remain = 0;
    //     if (equip_get_status(eq, dev_user, sizeof(dev_user), &dev_remain) == 0) {
    //         char msg[128];
    //         snprintf(msg, sizeof(msg),
    //                  "[DEBUG] device #%d: user=%s remain=%d\n",
    //                  eq->id, dev_user, dev_remain);
    //         send_all(cli->fd, msg);
    //     } else {
    //         send_all(cli->fd, "[DEBUG] device: ERROR\n");
    //     }
    //     return;
    // }
}

static void handle_driver_command(Client *cli, Equipment *eqs, int eq_count, const char *line)
{
    time_t now = time(NULL);
    char buf[LINE_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s", line);
    buf[sizeof(buf)-1] = '\0';
    char *cmd = strtok(buf, " ");
    if (!cmd) 
    {
        return;
    }
    if (strcmp(cmd, "QUERY_EQUIP") == 0) {
        char *equip_str = strtok(NULL, " ");
        if (!equip_str) return;
        int equip_id = atoi(equip_str);
        Equipment *eq = get_equipment_by_id(eqs, eq_count, equip_id);
        if (!eq) {
            send_all(cli->fd, "EQUIP_ERROR INVALID_EQUIP\n");
            return;
        }
        if (eq->state == EQUIP_IDLE) {
            char msg[64];
            snprintf(msg, sizeof(msg), "EQUIP_IDLE %d\n", eq->id);
            send_all(cli->fd, msg);
        } else if (eq->state == EQUIP_WAITING_CHECKIN) {
            int remain = (int)(eq->wait_deadline - now);
            if (remain < 0) 
            {
                remain = 0;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "EQUIP_WAIT %d %s %d\n", eq->id, eq->wait_user, remain);
            send_all(cli->fd, msg);
        } else if (eq->state == EQUIP_IN_USE) {
            int remain = (int)(eq->use_end_time - now);
            if (remain < 0) 
            {
                remain = 0;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "EQUIP_INUSE %d %s %d\n", eq->id, eq->use_user, remain);
            send_all(cli->fd, msg);
        }
    }
}
// ====== Threading ======
static void *timer_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        sleep(1);
        pthread_mutex_lock(&g_lock);
        for (int e = 0; e < EQUIP_COUNT; ++e) {
            update_equipment_state(&g_eqs[e], g_clients, MAX_CLIENTS);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}
// Clear client info when disconnected; free any reservation/usage
static void cleanup_client(Client *cli)
{
    if (cli->fd <= 0) 
    {
        return;
    }
    if (cli->role == ROLE_USER && cli->user_id[0] != '\0') {
        const char *uid = cli->user_id;
        broadcast_log(g_clients, MAX_CLIENTS, "DISCONNECT %s", uid);
        for (int e = 0; e < EQUIP_COUNT; ++e) {
            Equipment *eq = &g_eqs[e];
            if (eq->state == EQUIP_IN_USE &&strcmp(eq->use_user, uid) == 0) {
                broadcast_log(g_clients, MAX_CLIENTS,"DISCONNECT_RELEASE E%d %s USING", eq->id, uid);
                release_usage_and_promote(eq, g_clients, MAX_CLIENTS, 0);
            }
            if (eq->state == EQUIP_WAITING_CHECKIN &&strcmp(eq->wait_user, uid) == 0) {
                broadcast_log(g_clients, MAX_CLIENTS,"DISCONNECT_RELEASE E%d %s WAITING", eq->id, uid);
                eq->wait_user[0] = '\0';
                eq->wait_deadline = 0;
                equip_stop_countdown(eq);
                give_turn_to_next(eq, g_clients, MAX_CLIENTS);
            }
            remove_from_queue(eq, uid);
        }
    }
    close(cli->fd);
    cli->fd = 0;
    cli->role = ROLE_NONE;
    cli->user_id[0] = '\0';
    cli->ban_end = 0;
    cli->inbuf_len = 0;
}
//one thread per client
static void *client_thread(void *arg)
{
    Client *cli = (Client *)arg;
    while (g_running) {
        char buf[512];
        ssize_t n = recv(cli->fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            pthread_mutex_lock(&g_lock);
            cleanup_client(cli);
            pthread_mutex_unlock(&g_lock);
            break;
        }
        if (cli->inbuf_len + (int)n >= LINE_BUF_SIZE)
        {
            n = LINE_BUF_SIZE - 1 - cli->inbuf_len;
        }
        memcpy(cli->inbuf + cli->inbuf_len, buf, (size_t)n);
        cli->inbuf_len += (int)n;
        cli->inbuf[cli->inbuf_len] = '\0';
        char *start = cli->inbuf;
        char *newline;
        while ((newline = strchr(start, '\n')) != NULL) {
            *newline = '\0';
            char line[LINE_BUF_SIZE];
            snprintf(line, sizeof(line), "%s", start);
            line[sizeof(line)-1] = '\0';
            trim_newline(line);
            pthread_mutex_lock(&g_lock);
            if (cli->role == ROLE_NONE) {
                if (strncmp(line, "LOGIN ", 6) == 0) {
                    char *uid = line + 6;
                    snprintf(cli->user_id, USER_ID_LEN, "%.*s", USER_ID_LEN - 1, uid);
                    cli->user_id[USER_ID_LEN-1] = '\0';
                    cli->role = ROLE_USER;
                    send_all(cli->fd, "LOGIN_OK\n");
                    LOG_INFO("Server", "USER login: %s (fd=%d)", cli->user_id, cli->fd);
                } else if (strcmp(line, "DRIVER") == 0) {
                    cli->role = ROLE_DRIVER;
                    send_all(cli->fd, "DRIVER_OK\n");
                    LOG_INFO("Server", "DRIVER connected (fd=%d)",cli->fd);
                } else {
                    send_all(cli->fd, "INVALID_FIRST_LINE\n");
                    cleanup_client(cli);
                    pthread_mutex_unlock(&g_lock);
                    return NULL;
                }
            } else {
                if (cli->role == ROLE_USER) {
                    handle_user_command(cli, g_clients, MAX_CLIENTS, g_eqs, EQUIP_COUNT, line);
                } else if (cli->role == ROLE_DRIVER) {
                    handle_driver_command(cli, g_eqs, EQUIP_COUNT, line);
                }
            }
            pthread_mutex_unlock(&g_lock);
            start = newline + 1;
        }
        int remain = (int)(cli->inbuf + cli->inbuf_len - start);
        if (remain > 0) {
            memmove(cli->inbuf, start, (size_t)remain);
        }
        cli->inbuf_len = remain;
        cli->inbuf[cli->inbuf_len] = '\0';
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2 + EQUIP_COUNT * 2) {
        LOG_ERROR("Server", "Usage: %s <client_port> <equip0_ip> <equip0_port> <equip1_ip> <equip1_port>", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t client_port = (uint16_t)atoi(argv[1]);
    const char *equip_ips[EQUIP_COUNT];
    int equip_ports[EQUIP_COUNT];
    for (int i = 0; i < EQUIP_COUNT; ++i) {
        equip_ips[i] = argv[2 + i * 2];
        equip_ports[i] = atoi(argv[3 + i * 2]);
    }
    signal(SIGINT, handle_sigint);
    pthread_mutex_init(&g_lock, NULL);
    memset(g_clients, 0, sizeof(g_clients));
    memset(g_eqs, 0, sizeof(g_eqs));
    for (int i = 0; i < EQUIP_COUNT; ++i) {
        g_eqs[i].id = i;
        g_eqs[i].sock = -1;
        g_eqs[i].state = EQUIP_IDLE;
        if (equip_connect(&g_eqs[i], equip_ips[i], equip_ports[i]) < 0) {
            LOG_ERROR("Server", "Failed to connect to equipment #%d.", i);
            for (int j = 0; j < i; ++j) {
                if (g_eqs[j].sock >= 0) 
                {
                    close(g_eqs[j].sock);
                }
            }
            pthread_mutex_destroy(&g_lock);
            return EXIT_FAILURE;
        }
    }
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_thread, NULL);
    int listen_fd = create_listen_socket(client_port);
    if (listen_fd < 0) {
        g_running = 0;
        pthread_join(timer_tid, NULL);
        pthread_mutex_destroy(&g_lock);
        return EXIT_FAILURE;
    }
    LOG_INFO("Server", "Server listening on port %u (client) and connected to %d equipment drivers", client_port, EQUIP_COUNT);
    while (g_running) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int conn_fd = accept(listen_fd, (struct sockaddr*)&cliaddr, &clilen);
        if (conn_fd < 0) {
            if (errno == EINTR && !g_running) break;
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pthread_mutex_lock(&g_lock);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (g_clients[i].fd == 0) {
                slot = i;
                g_clients[i].fd = conn_fd;
                g_clients[i].role = ROLE_NONE;
                g_clients[i].user_id[0] = '\0';
                g_clients[i].ban_end = 0;
                g_clients[i].inbuf_len = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        if (slot < 0) {
            const char *msg = "Server full\n";
            send_all(conn_fd, msg);
            close(conn_fd);
        } else {
            LOG_INFO("Server", "New connection accepted (fd=%d)", conn_fd);
            pthread_t tid;
            pthread_create(&tid, NULL, client_thread, &g_clients[slot]);
            pthread_detach(tid);
        }
    }
    close(listen_fd);
    g_running = 0;
    // close driver sockets
    for (int i = 0; i < EQUIP_COUNT; ++i) {
        if (g_eqs[i].sock >= 0) 
        {
            close(g_eqs[i].sock);
        }
    }
    pthread_join(timer_tid, NULL);
    pthread_mutex_destroy(&g_lock);
    LOG_INFO("Server", "Server shutdown.");
    return 0;
}

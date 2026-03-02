#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define BUFSIZE 256

enum Shop { SHOP_DESSERT = 0, SHOP_BEVERAGE = 1, SHOP_DINER = 2 };

typedef struct {
    int shop_set;              
    enum Shop shop;            
    int cookie_qty;
    int cake_qty;
    int tea_qty;
    int boba_qty;
    int fried_qty;
    int soup_qty;
    int total_money;
} OrderState;

int shop_distance[3] = {3, 5, 8};  // Dessert:3km, Beverage:5km, Diner:8km

int meal_to_shop(const char *meal) {
    if (strcmp(meal, "cookie") == 0 || strcmp(meal, "cake") == 0)
        return SHOP_DESSERT;
    else if (strcmp(meal, "tea") == 0 || strcmp(meal, "boba") == 0)
        return SHOP_BEVERAGE;
    else if (strcmp(meal, "fried-rice") == 0 || strcmp(meal, "Egg-drop-soup") == 0)
        return SHOP_DINER;
    else
        return -1;
}

int meal_price(const char *meal, int amount) {
    if (strcmp(meal, "cookie") == 0)        return 60 * amount;
    if (strcmp(meal, "cake") == 0)          return 80 * amount;
    if (strcmp(meal, "tea") == 0)           return 40 * amount;
    if (strcmp(meal, "boba") == 0)          return 70 * amount;
    if (strcmp(meal, "fried-rice") == 0)    return 120 * amount;
    if (strcmp(meal, "Egg-drop-soup") == 0) return 50 * amount;
    return 0;
}

void build_order_string(const OrderState *st, char *buf, size_t bufsize) {
    buf[0] = '\0';
    if (!st->shop_set) return;
    switch (st->shop) {
        case SHOP_DESSERT:
            if (st->cookie_qty > 0 && st->cake_qty > 0) {
                //display for cookie and cake
                snprintf(buf, bufsize, "cookie %d|cake %d",st->cookie_qty, st->cake_qty);
            } else if (st->cookie_qty > 0) {
                //display for cookie
                snprintf(buf, bufsize, "cookie %d", st->cookie_qty);
            } else if (st->cake_qty > 0) {
                //display for  cake
                snprintf(buf, bufsize, "cake %d", st->cake_qty);
            }
            break;
        case SHOP_BEVERAGE:
            if (st->tea_qty > 0 && st->boba_qty > 0) {
                snprintf(buf, bufsize, "tea %d|boba %d",st->tea_qty, st->boba_qty);
            } else if (st->tea_qty > 0) {
                snprintf(buf, bufsize, "tea %d", st->tea_qty);
            } else if (st->boba_qty > 0) {
                snprintf(buf, bufsize, "boba %d", st->boba_qty);
            }
            break;
        case SHOP_DINER:
            if (st->fried_qty > 0 && st->soup_qty > 0) {
                snprintf(buf, bufsize, "fried-rice %d|Egg-drop-soup %d",st->fried_qty, st->soup_qty);
            } else if (st->fried_qty > 0) {
                snprintf(buf, bufsize, "fried-rice %d", st->fried_qty);
            } else if (st->soup_qty > 0) {
                snprintf(buf, bufsize, "Egg-drop-soup %d", st->soup_qty);
            }
            break;
    }
}

void send_msg(int connfd, const char *msg) {
    char sendbuf[BUFSIZE];
    memset(sendbuf, 0, BUFSIZE);
    //Copy string to buffer
    strncpy(sendbuf, msg, BUFSIZE - 1);
    if (write(connfd, sendbuf, BUFSIZE) == -1) {
        perror("Error: write()\n");
        exit(1);
    }
}

void CtrlCHandler(int signum) {
    (void)signum;
    exit(0);
}

int main(int argc, char *argv[]) {
    int sockfd, connfd;
    struct sockaddr_in servaddr;
    char recv_buf[BUFSIZE];
    int rn;
    if (argc != 2)
    {
        fprintf(stderr,"Usage: %s <PORT>\n", argv[0]);
        exit(1);
    }
    //build socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(atoi(argv[1]));
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Error: bind()\n");    
        exit(1);
    }
    if (listen(sockfd, 10) < 0)
    {
        perror("Error: listen()\n");
        exit(1);
    }
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        connfd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
        if (connfd == -1){
            perror("Error: accept()\n");
            exit(1);
        }
        OrderState st;
        //initial orderstate all 0
        memset(&st, 0, sizeof(st));
        while (1) {
            //Ctrl+c
            signal(SIGINT, CtrlCHandler);
            //clear receive buffer
            memset(recv_buf, 0, BUFSIZE);
            //Read a maximum of 256 bytes of instructions from the client
            rn = read(connfd, recv_buf, BUFSIZE);
            if (rn == -1){
                perror("Error: read()\n");
                exit(1);
            }
            if (rn == 0) {
                break;
            }
            //judge string whether is same as shop list from client
            if (strcmp(recv_buf, "shop list") == 0) {
                //Server transmit menu to the client
                send_msg(connfd,
                         "Dessert Shop:3km\n- cookie:60$|cake:80$\n"
                         "Beverage Shop:5km\n- tea:40$|boba:70$\n"
                         "Diner:8km\n- fried-rice:120$|Egg-drop-soup:50$");
            }
            else if (strcmp(recv_buf, "confirm") == 0) {
                //The user did not order any food
                if (!st.shop_set) {
                    send_msg(connfd, "Please order some meals");
                //The user has placed an order
                } else {
                    send_msg(connfd, "Please wait a few minutes...");
                    sleep(shop_distance[st.shop]);
                    char pay_msg[BUFSIZE];
                    //The amount due is placed in the message and written to the buffer pay_msg
                    snprintf(pay_msg, sizeof(pay_msg),"Delivery has arrived and you need to pay %d$",st.total_money);
                    send_msg(connfd, pay_msg);
                    break;
                }
            }
            else if (strcmp(recv_buf, "cancel") == 0) {
                break;
            }
            else {
                char cmd[16], meal[32];
                int amount = 0;
                if (sscanf(recv_buf, "%15s %31s %d", cmd, meal, &amount) != 3) {
                    continue;
                }
                int shop = meal_to_shop(meal);
                //Judging a restaurant by its food
                if (!st.shop_set) {
                    if (shop < 0) continue; 
                    st.shop_set = 1;
                    st.shop = (enum Shop)shop;
                }
                //Cross-store orders are not allowed
                if (shop != st.shop) {
                    char order_str[BUFSIZE];
                    build_order_string(&st, order_str, sizeof(order_str));
                    send_msg(connfd, order_str);
                    continue;
                }
                st.total_money += meal_price(meal, amount);
                switch (shop) {
                    case SHOP_DESSERT:
                        if (strcmp(meal, "cookie") == 0)
                            st.cookie_qty += amount;
                        else
                            st.cake_qty += amount;
                        break;
                    case SHOP_BEVERAGE:
                        if (strcmp(meal, "tea") == 0)
                            st.tea_qty += amount;
                        else
                            st.boba_qty += amount;
                        break;
                    case SHOP_DINER:
                        if (strcmp(meal, "fried-rice") == 0)
                            st.fried_qty += amount;
                        else
                            st.soup_qty += amount;
                        break;
                }
                char order_str[BUFSIZE];
                build_order_string(&st, order_str, sizeof(order_str));
                send_msg(connfd, order_str);
            }
        }
        close(connfd);
    }
    close(sockfd);
    return 0;
}

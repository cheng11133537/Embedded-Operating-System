#define _POSIX_C_SOURCE 199309L   
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

static void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000L;
    (void)nanosleep(&ts, NULL);   
}

// 7-seg
#define GPIO_14 (14)  // a
#define GPIO_15 (15)  // b
#define GPIO_18 (18)  // c
#define GPIO_23 (23)  // d
#define GPIO_24 (24)  // e
#define GPIO_22 (22)  // f
#define GPIO_17 (17)  // g
// LED 
#define GPIO_4  (4) 
#define GPIO_9  (9)
#define GPIO_5  (5)
#define GPIO_6  (6)
#define GPIO_16 (16)
#define GPIO_21 (21)
#define GPIO_26 (26)
#define GPIO_10 (10)  

static int seg_pins[7] = {GPIO_14, GPIO_15, GPIO_18, GPIO_23, GPIO_24, GPIO_22, GPIO_17};
static int led_pins[8] = {GPIO_4, GPIO_9, GPIO_5,  GPIO_6,  GPIO_16, GPIO_21, GPIO_26, GPIO_10};

static const unsigned char seg_map[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};


static int gpio_export(int gpio){
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", gpio);
    ssize_t rc = write(fd, buf, len);
    int saved = errno;
    close(fd);
    if (rc < 0 && saved != EBUSY) return -1;
    return 0;
}
static int gpio_set_dir(int gpio, const char* dir){
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int rc = write(fd, dir, strlen(dir));
    close(fd);
    return (rc < 0) ? -1 : 0;
}
static int gpio_set_val(int gpio, int val){
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    const char *s = val ? "1" : "0";
    int rc = write(fd, s, 1);
    close(fd);
    return (rc < 0) ? -1 : 0;
}

static void gpio_prepare_out(const int* pins, int n){
    for (int i=0;i<n;++i){
        gpio_export(pins[i]);
        sleep_us(100000);
        gpio_set_dir(pins[i], "out");
        sleep_us(10000);
        gpio_set_val(pins[i], 0);
    }
}

static void seg_set_mask(unsigned char m){
    for (int i=0;i<7;++i)
        gpio_set_val(seg_pins[i], (m >> i) & 1);
}

static void seg_display(int amount){
    int pow10 = 1, t = amount;
    while (t >= 10){ 
        t /= 10; 
        pow10 *= 10; 
    }
    while (pow10 > 0){
        int digit = (amount / pow10) % 10;
        seg_set_mask(seg_map[digit]);
        pow10 /= 10;
        if (pow10 > 0) sleep_us(500000);
    }
}

static void leds_show_first_n(int n){
    for (int i=0; i<8; ++i)
        gpio_set_val(led_pins[i], (i < n ? 1 : 0));
}

static void led_display(int ordershop){
    int distance = 0;
    if(ordershop == 1) {
        distance = 3;
    } else if (ordershop == 2) {
        distance = 5;
    } else if (ordershop == 3) {
        distance = 8; 
    }
    for (int cur = distance; cur >= 0; --cur){
        leds_show_first_n(cur);
        sleep(1);
    }
}

static int order_shop(const char* shopname,const char* item1, int price1, int* qty1,const char* item2, int price2, int* qty2)
{
    *qty1 = *qty2 = 0;
    for (;;) {
        int sel = 0, n = 0;
        printf("\n[%s]\n", shopname);
        printf("Please choose from 1~4\n");
        printf("1. %s: $%d\n", item1, price1);
        printf("2. %s: $%d\n", item2, price2);
        printf("3. confirm\n");
        printf("4. cancel\n> ");
        if (scanf("%d%*c", &sel) != 1) continue;

        if (sel == 1){
            printf("How many? ");
            if (scanf("%d%*c", &n) == 1) *qty1 += n;
        } else if (sel == 2){
            printf("How many? ");
            if (scanf("%d%*c", &n) == 1) *qty2 += n;
        } else if (sel == 3){
            int total = (*qty1)*price1 + (*qty2)*price2;
            printf("\n=== ORDER SUMMARY ===\n");
            if (*qty1) printf("%s x %d = $%d\n", item1, *qty1, *qty1*price1);
            if (*qty2) printf("%s x %d = $%d\n", item2, *qty2, *qty2*price2);
            printf("Total: $%d\n======================\n", total);
            return total;
        } else if (sel == 4){
            *qty1 = *qty2 = 0;
            return -1;
        }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    gpio_prepare_out(seg_pins, 7);
    gpio_prepare_out(led_pins, 8);

    for (;;) {
        int main_sel = 0;
        printf("\nplease select number:\n1: shop list\n2: order\n> ");
        if (scanf("%d%*c", &main_sel) != 1) continue;

        if (main_sel == 1){
            printf("Dessert shop: 3km\nBeverage shop: 5km\nDiner: 8km\n");
            printf("(press Enter to go back)\n");
            getchar();
        } 
        else if (main_sel == 2){
            int ordershop = 0;
            printf("\nPlease choose from 1~3\n");
            printf("1. Dessert shop\n2. Beverage shop\n3. Diner\n> ");
            if (scanf("%d%*c", &ordershop) != 1) continue;

            int amount;
            if (ordershop == 1){
                int q1=0, q2=0;
                amount = order_shop("Dessert shop","cookie",60,&q1,"cake",80,&q2);
            } else if (ordershop == 2){
                int q1=0, q2=0;
                amount = order_shop("Beverage shop","tea",40,&q1,"boba",70,&q2);
            } else {
                int q1=0, q2=0;
                amount = order_shop("Diner","fried rice",120,&q1,"egg-drop soup",50,&q2);
            }

            if (amount == -1)
                printf("Order canceled. Back to main menu.\n");
            else {
                seg_display(amount);
                printf("Please wait for a few minutes...\n");
                led_display(ordershop);
                puts("please pick up your meal");
                printf("(press Enter to go back)\n");
                getchar();
            }
        }
    }
}

// Windows 版 towel client：連到 Python server 收 "HAS" / "NO"
// 只記錄最近 3 秒內的結果，當 3 秒視窗內 HAS 比例 > 2/3 時，印出 HasTowel 然後結束。
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")    // MSVC 用，MinGW 會忽略沒關係

#define BUF_SIZE        128
#define MAX_SAMPLES     2048          // 最多暫存幾筆結果，足夠就好
#define WINDOW_SECONDS  3.0           // 滑動視窗長度（秒）
#define RATIO_THRESHOLD (2.0/3.0)     // 門檻 2/3
#define MIN_SAMPLES     40            // 至少累積 40 筆才允許判定

typedef struct {
    double t;   // 收到這筆結果的時間（程式啟動後經過的秒數）
    int has;    // 1 = HAS, 0 = NO
} Sample;

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    // 初始化 Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port   = htons(port);

    // 把字串 IP 轉成 address
    unsigned long addr = inet_addr(server_ip);
    if (addr == INADDR_NONE) {
        fprintf(stderr, "inet_addr failed for %s\n", server_ip);
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    servaddr.sin_addr.s_addr = addr;


    if (connect(sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect failed: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("Connected to %s:%d\n", server_ip, port);
    printf("Client running. Will print HasTowel when HAS ratio > 2/3 over last %.1f seconds.\n",
           WINDOW_SECONDS);

    char buf[BUF_SIZE];
    Sample samples[MAX_SAMPLES];
    int sample_count = 0;

    // 記錄程式開始時間，用來換算相對秒數
    double t0 = (double)clock() / CLOCKS_PER_SEC;

    
    while (1) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n < 0) {
                fprintf(stderr, "recv failed: %d\n", WSAGetLastError());
            } else {
                printf("Server closed connection\n");
            }
            break;
        }
        buf[n] = '\0';

        // 可能一次收到多行 "HAS\nNO\n..."，逐行處理
        char *saveptr = NULL;
        char *line = strtok_s(buf, "\n", &saveptr);
        while (line != NULL) {
            int has = 0;
            if (strstr(line, "HAS") != NULL) {
                has = 1;
            } else if (strstr(line, "NO") != NULL) {
                has = 0;
            } else {
                // 非預期內容就略過
                line = strtok_s(NULL, "\n", &saveptr);
                continue;
            }

            // 目前時間（相對程式啟動）
            double now = (double)clock() / CLOCKS_PER_SEC - t0;
            double window_start = now - WINDOW_SECONDS;

            // 先把「已存在 samples 裡、但時間 < window_start」的丟掉
            int new_count = 0;
            for (int i = 0; i < sample_count; ++i) {
                if (samples[i].t >= window_start) {
                    if (new_count != i) {
                        samples[new_count] = samples[i];
                    }
                    new_count++;
                }
            }

            // 把這一筆新的結果加進來
            if (new_count >= MAX_SAMPLES) {
                // buffer 滿了，丟掉最舊那一筆
                for (int i = 1; i < new_count; ++i) {
                    samples[i - 1] = samples[i];
                }
                new_count--;
            }

            samples[new_count].t   = now;
            samples[new_count].has = has;
            new_count++;

            sample_count = new_count;

            // 計算這個 3 秒窗內 HAS 比例
            int has_count = 0;
            for (int i = 0; i < sample_count; ++i) {
                if (samples[i].has) has_count++;
            }

            double ratio = 0.0;
            if (sample_count > 0) {
                ratio = (double)has_count / (double)sample_count;
            }

            // 如果比例達標，就顯示一次 HasTowel 然後結束程式
            if (now >= WINDOW_SECONDS && sample_count >= MIN_SAMPLES && ratio >= RATIO_THRESHOLD) {
                printf("\n[RESULT] Last %.1f seconds: HAS ratio = %.2f (%d samples) -> HasTowel\n",
                       WINDOW_SECONDS, ratio, sample_count);
                closesocket(sock);
                WSACleanup();
                return 0;
            }

            // 下一行
            line = strtok_s(NULL, "\n", &saveptr);
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

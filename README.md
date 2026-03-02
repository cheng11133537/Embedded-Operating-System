#  Embedded Operating System (EOS)

> 本專案收錄了「嵌入式作業系統 (Embedded Operating System)」課程的完整核心與應用實作。

本課程的實作涵蓋了作業系統最核心的幾個維度：從底層的 Linux Kernel 核心優化與重編譯、字元裝置驅動程式開發，一路延伸至應用層的 Multi-process 併發伺服器架構、進階的行程間通訊 (IPC) 與同步機制，最終整合為一套具備 IoT 邊緣裝置通訊能力的「分散式器材預約與管理系統」。

##  開發與測試環境

* **硬體平台:** Raspberry Pi (樹莓派)
* **作業系統:** Linux (Raspberry Pi OS) / Custom Real-Time Kernel
* **開發語言:** C / C++ / Shell Script
* **核心技術:** Socket Programming, System V IPC, Linux Device Drivers, POSIX Signals
* **建置與測試工具:** GCC, Makefile, Tmux, Netcat (nc)

---

##  專案導覽 (Project Overview)

###  Lab 2: Kernel Customization & Real-Time Patch
本實驗深入 Linux 核心，對樹莓派的作業系統進行瘦身與 Real-Time 升級。

* **技術亮點:** * **Kernel Shrinking:** 透過 `menuconfig` 移除針對快閃記憶體優化的 `F2FS` 支援，以及不必要的 `Stack Protector` 溢位偵測機制，成功縮減了 OS Image size 並降低記憶體開銷。
  * **RT-Preempt Patch:** 成功為核心打上 `4.19.127-rt54-v8+` 補丁，將系統的排程模型更改為 Fully Preemptible Kernel (RT)，賦予系統即時作業能力。

###  Lab 3: 七段顯示器硬體驅動程式 (Character Device Driver)
本實驗實作了一個標準的 Linux 字元裝置驅動程式，用於控制樹莓派的 GPIO 腳位以點亮七段顯示器，並與 User-space 應用程式進行互動。

* **技術亮點:**
  * **Driver API 實作:** 自行定義了 `file_operations`，並透過 `copy_from_user` 安全地接收來自應用層的字元資料。
  * **GPIO 映射控制:** 內建 `0-9` 與 `A-F` 的 16 進位字元對應表 (`seg_map`)，精準控制 GPIO (14, 15, 17, 18, 22, 23, 24) 腳位的高低電位，達成學號字串的跑馬燈顯示效果。

###  Lab 4: 東方快車 (Concurrent Socket Server)
本實驗開發了一支基於 TCP/IP 的併發伺服器，能夠同時處理多個 Client 端的連線請求。

* **技術亮點:**
  * **Multi-process 併發處理:** 利用 `socket()`, `bind()`, `listen()` 建立伺服器後，透過 `fork()` 系統呼叫為每一個新建立的連線 (`accept()`) 分配獨立的子行程 (Child Process) 進行服務。
  * ** Zombie Process 防護:** 完美實作了 Signal Handling，透過捕捉 `SIGCHLD` 信號並搭配 `waitpid(-1, NULL, WNOHANG)`，確保子行程結束後資源能被作業系統正確回收，避免記憶體洩漏。

###  Lab 5: Web ATM (IPC Semaphores & Synchronization)
本實驗模擬了銀行的 ATM 系統，解決了在多個行程同時對同一個帳戶進行「存款」與「提款」時所發生的 Race Condition (競爭危害) 問題。

* **技術亮點:**
  * **System V Semaphore:** 完美運用了作業系統的號誌機制 (`semget`, `semctl`)，自行封裝了 `P()` (Wait/Lock) 與 `V()` (Signal/Unlock) 函式。
  * **Critical Section 保護:** 確保在修改共享變數 `balance` 時的互斥性 (Mutual Exclusion)。並透過自動化測試腳本 (`demo.sh`) 使用 Tmux 同時啟動 4 個 Client 進行高壓存提款測試，驗證同步機制的絕對穩健性。

###  Lab 6: 終極密碼 (Shared Memory & Signals)
本實驗開發了一套由兩個獨立行程 (`game.c` 與 `guess.c`) 互相配合的猜數字遊戲，展現了極高效率的行程間通訊技巧。

* **技術亮點:**
  * **極速資料交換:** 建立一塊 System V 共享記憶體 (Shared Memory) 作為雙方傳遞猜測數字與比對結果的橋樑。
  * **非同步信號觸發:** 結合 `SIGUSR1` 信號進行跨行程通知；同時利用 `setitimer` 搭配 `SIGALRM` 信號，實作了每秒精準觸發一次的「自動化二元搜尋 (Binary Search)」演算法。
  * **CPU 資源優化:** 透過 `pause()` 系統呼叫讓行程在等待期間進入休眠，避免 Busy Waiting，達到零無謂耗能的完美同步。

---

##  Final Project: 分散式器材預約與管理系統 (IoT Reservation System)
本專題為 EOS 課程的集大成之作，設計並實作了一套具備商業級系統雛形的物聯網 (IoT) 器材管理架構，包含了 Client 端介面、Server 端業務邏輯，以及 Edge Device (邊緣硬體) 的狀態模擬。

* **技術亮點:**
  * **Custom Application Protocol:** 制定了嚴謹的 C/S 架構通訊協定標準 (例如 `STARTCOUNTDOWN`, `GETSTATUS`, `userMissed`)，徹底解決字串解析的模糊地帶，展現高度的軟體工程素養。
  * **I/O Multiplexing:** Server 端利用 `select()` 函式實現單一行程同時監聽多個 Socket 檔案描述符 (File Descriptors)，高效處理來自 Client 的預約請求以及器材端的主動狀態回報。
  * **Edge Device 狀態機:** 器材端 (`driver.c`) 實作了完整的狀態轉移邏輯，能根據 Server 的指令啟動倒數、強制停止，並具備使用者未依約報到時的「逾時自動回報與懲罰觸發」機制。

---

##  專案目錄結構 (Repository Layout)

```text
Embedded-Operating-System/
├── Lab2_RT_Kernel/           # Kernel 優化與 Real-Time 補丁報告
├── Lab3_Device_Driver/       # 七段顯示器 GPIO 驅動程式與 User 應用
├── Lab4_Concurrent_Server/   # Multi-process TCP 併發伺服器
├── Lab5_IPC_Semaphore/       # 解決 Race Condition 的 Web ATM
├── Lab6_Shared_Memory/       # 結合信號與共享記憶體的二元搜尋演算法
└── Final_Project/            # IoT 器材預約與狀態監控系統
    ├── server.c              
    ├── client.c             
    ├── driver.c              
    └── driver-api-protocol.md 

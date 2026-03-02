這是一份為了讓後端 (Server) 與 器材端 (Device/Equipment) 開發人員能夠順暢溝通所撰寫的 **Socket 通訊協定文件 (Protocol Specification)**。

這份文件將模糊的 C 語言格式字串轉化為具體的通訊標準，定義了連接方式、封包結構、以及錯誤處理機制。

---

# 器材控制通訊協定 (Equipment Control Protocol) v2.0

## 1. 概述 (Overview)

本文件定義 Server 端與器材端之間的 Socket 通訊標準。

* **通訊架構**：雙向通訊

  1. **控制指令**：Server → 器材 (控制運作、查詢狀態)
  2. **主動回報**：器材 → Server (回報結束、回報違規)
* **傳輸層協定**：TCP/IP
* **編碼格式**：ASCII / UTF-8
* **指令結尾 (Delimiter)**：每一條指令與回應皆以換行符號 `\n` (0x0A) 作為結束
* **參數分隔**：指令與參數之間以 **空白鍵 (Space)** 分隔

---

## 指令清單 (Command List)

* **Server 下達指令 (Server → Driver)**

  * 2.1. [取得器材狀態 (getStatus)](#21-取得器材狀態-getstatus)
  * 2.2. [開始倒數計時 (startCountdown)](#22-開始倒數計時-startcountdown)
  * 2.3. [停止倒數計時 (stopCountdown)](#23-停止倒數計時-stopcountdown)

* **Driver 主動回報 (Driver → Server)**

  * 3.1. [使用者完成 (userFinished)](#31-使用者完成-userfinished)
  * 3.2. [使用者逾時未到 (userMissed)](#32-使用者逾時未到-usermissed)

---

## 2. Server 下達指令 (Server Commands)
由 Server 主動發起，器材端接收並回應。

### 2.1 取得器材狀態 (getStatus)
Server 詢問當前器材的使用者及剩餘時間。

*   **指令 (Request):**
    ```text
    getStatus
    ```
*   **回應 (Response):**
    ```text
    <user_id> <time_remain>
    ```
*   **參數說明:**
    *   `user_id`: 字串。當前使用者 ID。若器材閒置中 (Idle)，則回傳 `NONE`。
    *   `time_remain`: 整數。剩餘秒數。若器材閒置中，則回傳 `0`。
*   **範例:**
    *   有人使用中: `User123 45` (代表 User123 正在使用，剩 45 秒)
    *   閒置中: `NONE 0`

---

### 2.2 開始倒數計時 (startCountdown)
Server 指示器材鎖定並開始運作指定時間。

*   **指令 (Request):**
    ```text
    startCountdown <seconds> <user_id>
    ```
*   **參數說明:**
    *   `seconds`: 整數。倒數的秒數 (例如：60)。
*   **回應 (Response):**
    ```text
    ACK <status_code>
    ```
*   **範例:**
    *   `startCountdown 60` -> `ACK 1` (成功開始)

---

### 2.3 停止倒數計時 (stopCountdown)
Server 強制停止器材運作（例如使用者提前結束、或管理員強制關閉）。

*   **指令 (Request):**
    ```text
    stopCountdown
    ```
*   **回應 (Response):**
    ```text
    ACK <status_code>
    ```
*   **範例:**
    *   `stopCountdown` -> `ACK 1` (成功停止)

---

## 3. 器材主動回報 (Device Notifications)
由器材端 (Driver) 主動發起，通知 Server 特定事件已發生。Server 收到後需回傳 ACK 以示收到。

### 3.1 使用者完成 (userFinished)
當使用者在倒數結束前主動完成動作，或倒數時間自然結束，器材發送此訊號通知 Server 該次使用已結束，可以安排下一位。

*   **通知 (Notification):**
    ```text
    userFinished <user_id>
    ```
*   **參數說明:**
    *   `user_id`: 字串。剛結束使用的使用者 ID。
*   **Server 回應 (Response):**
    ```text
    ACK 1
    ```
*   **範例:**
    *   `userFinished User123` -> Server 紀錄完成並準備下一位。

### 3.2 使用者逾時未到 (userMissed)
當 Server 下達開始指令後，若使用者在規定時間內（例如 1 分鐘）未進行報到或開始操作，器材發送此訊號通知 Server 進行違規懲罰。

*   **通知 (Notification):**
    ```text
    userMissed <user_id>
    ```
*   **參數說明:**
    *   `user_id`: 字串。該名未到的使用者 ID。
*   **Server 回應 (Response):**
    ```text
    ACK 1
    ```
*   **範例:**
    *   `userMissed UserBadGuy` -> Server 標記該用戶違規 (Penalty)。

---

## 4. 狀態碼定義 (Status Codes)

針對 `ACK` 回應的 `status_code` 定義如下：

| 代碼 (Code) | 意義 (Meaning) | 說明 |
| :--- | :--- | :--- |
| **1** | **Success (成功)** | 指令已成功接收並執行。 |
| **0** | **Failure (失敗)** | 指令執行失敗（例如：器材已在使用中無法 Start、或器材早已停止）。 |

---

## 5. 通訊範例 (Communication Sequence)

以下模擬 Server 與 器材 之間的互動流程。 `->` 代表 Server 發送， `<-` 代表器材回應。

### 情境 A：正常開始與結束
```text
(Server 想要查詢狀態)
-> getStatus\n
<- NONE 0\n                (器材回報：目前無人使用)

(Server 下達開始，設定 60 秒，指定 UserA)
-> startCountdown 60 UserA\n
<- ACK 1\n                  (器材回報：成功開始)

(過了 10 秒，Server 再次查詢狀態)
-> getStatus\n
<- UserA 50\n               (器材回報：UserA 使用中，剩餘 50 秒)

(UserA 在 40 秒時按下了結束按鈕，器材主動回報)
<- userFinished UserA\n
-> ACK 1\n                  (Server 確認收到)
```

### 情境 B：使用者遲到違規
```text
(Server 下達開始，指定 UserB)
-> startCountdown 60 UserB\n
<- ACK 1\n

(器材端偵測：過了 60 秒 UserB 都沒有動作)
<- userMissed UserB\n       (器材回報：此人沒來)
-> ACK 1\n                  (Server 確認收到，並對 UserB 懲罰)
```

### 情境 C：強制停止
```text
(Server 下達強制停止)
-> stopCountdown\n
<- ACK 1\n                  (器材回報：已停止)

(Server 確認狀態)
-> getStatus\n
<- NONE 0\n
```

### 情境 D：錯誤處理 (器材已被佔用)
```text
(Server 嘗試開始，但器材正在運作中)
-> startCountdown 30\n
<- ACK 0\n                  (器材回報：失敗，可能因目前忙碌中)
```

---

## 5. 開發注意事項 (Implementation Notes)

1.  **TCP Keep-Alive**: 建議開啟 TCP Keep-Alive 機制，或實作 Heartbeat (心跳包)，以確保連線中斷時能即時偵測。
2.  **Buffer Handling**: 接收端在解析 socket buffer 時，請務必以 `\n` 為切割點。因為 TCP 會有黏包 (Packet sticking) 狀況，可能會一次收到兩條指令 (例如 `getStatus\ngetStatus\n`)，程式需能透過迴圈處理。
3.  **User ID 格式**: 請雙方確認 `user_id` 是否包含空白鍵。本協定以空白鍵為分隔符，若 `user_id` 可能包含空白 (如 "John Doe")，建議回應格式改為 JSON 字串或是以底線取代空白。
4.  **非同步處理**: 器材端的 `userFinished` 與 `userMissed` 是主動發送的，Server 端需具備「隨時接收訊息」的能力，而不僅僅是 Request-Response 架構。
5.  **重試機制 (選擇性)**: 若器材發送 `userFinished` 後未收到 Server 的 `ACK` (例如網路斷線)，建議器材端實作簡單的重試機制或將紀錄暫存，待連線恢復後補傳。
---

### 💡 Gemini 給我的建議
如果你們的 User ID 比較複雜（例如包含特殊符號），我建議在 `getStatus` 的回傳部分考慮改用簡單的 **JSON** 格式，例如：
`{"user": "user_01", "time": 45}`
這樣擴充性會更好，但如果只是簡單的嵌入式系統，目前的純文字空白分隔格式效能最好且最容易實作。

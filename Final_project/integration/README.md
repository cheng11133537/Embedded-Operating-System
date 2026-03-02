
這份 `server.c` 程式碼實作了一個 **健身房器材預約系統** 的核心伺服器。它負責處理兩類客戶端連線：一般使用者（User）與器材驅動程式（Driver/Equipment）。

以下是根據程式碼邏輯整理出的完整 **通訊協定 (Protocol)** 解析。

-----

### 協定概觀 (Protocol Overview)

  * **傳輸層**：TCP Socket。
  * **格式**：純文字 (ASCII text)，以換行符號 `\n` (或 `\r\n`) 結尾。
  * **參數分隔**：空格 (Space)。
  * **指令處理**：伺服器一次讀取一行，解析第一個單詞作為指令 (Command)。

-----

### 1\. 連線與驗證階段 (Handshake & Authentication)

當 Client 建立 TCP 連線後，發送的第一行訊息決定其身分（使用者 App 或 器材硬體）。

| 發送方 | 指令格式 | 說明 | 伺服器回應 (成功) | 伺服器回應 (失敗) |
| :--- | :--- | :--- | :--- | :--- |
| **User** | `LOGIN <user_id>` | 使用者登入 (ID長度上限32) | `LOGIN_OK` | `INVALID_FIRST_LINE` (斷線) |
| **Driver** | `DRIVER` | 宣告自己為器材驅動程式 | `DRIVER_OK` | `INVALID_FIRST_LINE` (斷線) |

-----

### 2\. 使用者指令協定 (Client -\> Server)

這些指令由以 `LOGIN` 成功的使用者發送。

#### A. 預約器材 (RESERVE)

請求預約指定 ID 的器材。

  * **指令**：`RESERVE <equip_id>`
  * **回應**：

| 情境 | 回應格式 | 說明 |
| :--- | :--- | :--- |
| **成功 (無需排隊)** | `RESERVE_OK <id> FIRST <seconds>` | 預約成功，目前無人使用，獲得 `<seconds>` 秒報到時間。 |
| **成功 (需排隊)** | `RESERVE_OK <id> QUEUE <priority>` | 預約成功，排在隊伍第 `<priority>` 順位。 |
| **失敗 (無效器材)** | `RESERVE_FAIL INVALID_EQUIP` | 器材 ID 不存在。 |
| **失敗 (被停權)** | `RESERVE_FAIL <id> BANNED <seconds>` | 使用者被停權中，剩餘 `<seconds>` 秒。 |
| **失敗 (重複)** | `RESERVE_FAIL <id> DUPLICATE` | 使用者已在該器材的隊伍或使用中。 |
| **失敗 (達上限)** | `RESERVE_FAIL <id> LIMIT <count>` | 同時佔用器材數已達上限。 |
| **失敗 (隊列滿)** | `RESERVE_FAIL <id> FULL` | 該器材排隊人數已滿。 |

#### B. 取消預約 (CANCEL)

取消正在排隊或等待報到的狀態。

  * **指令**：`CANCEL <equip_id>`
  * **回應**：

| 情境 | 回應格式 | 說明 |
| :--- | :--- | :--- |
| **成功 (取消報到等待)** | `CANCEL_OK <id> WAITING` | 放棄報到機會 (Turn)，換下一位。 |
| **成功 (取消排隊)** | `CANCEL_OK <id> QUEUED` | 從排隊隊伍中移除。 |
| **失敗** | `CANCEL_FAIL INVALID_EQUIP` | 器材 ID 錯誤。 |
| **失敗** | `CANCEL_FAIL <id> NOT_FOUND` | 找不到該使用者的預約紀錄。 |

#### C. 查詢器材狀態 (QUERY\_EQUIP)

查詢特定器材目前的狀況。

  * **指令**：`QUERY_EQUIP <equip_id>`
  * **回應**：

| 情境 | 回應格式 |
| :--- | :--- |
| **閒置** | `EQUIP_IDLE <id>` |
| **使用中** | `EQUIP_INUSE <id> <user_id> <remain_seconds>` |
| **錯誤** | `EQUIP_ERROR <id>` 或 `EQUIP_ERROR INVALID_EQUIP` |

#### D. 查詢自身狀態 (QUERY\_USER)

查詢使用者目前所有的活動狀態。

  * **指令**：`QUERY_USER`
  * **回應** (依優先順序回傳第一條符合的狀態，若無則回傳 NORMAL)：

| 狀態 | 回應格式 |
| :--- | :--- |
| **被停權** | `USER_STATUS BANNED <remain_seconds>` |
| **正在使用某器材** | `USER_STATUS USING <id> <remain_seconds>` |
| **獲得報到權** | `USER_STATUS WAITING_CHECKIN <id> <remain_seconds>` |
| **排隊中** | `USER_STATUS QUEUED <id> <priority>` |
| **無活動** | `USER_STATUS NORMAL` |

#### E. 回覆續用詢問 (CONTINUE\_RESPONSE)

當使用者剛用完器材，系統詢問是否續用時的回覆。

  * **指令**：`CONTINUE_RESPONSE <equip_id> <0|1>` (0=不續用, 1=續用)
  * **回應** (僅在失敗時回傳，成功則觸發 Notification)：
      * `CONTINUE_FAIL MISSING_ARGS`
      * `CONTINUE_FAIL INVALID_EQUIP`
      * `CONTINUE_FAIL <id> INVALID_CHOICE`
      * `CONTINUE_FAIL <id> INVALID_STATE` (並非在等待續用回覆的狀態)

#### F. 報到/登出 (CHECKIN/CHECKOUT)

  * **指令**：`CHECKIN` 或 `CHECKOUT`
  * **回應**：`CHECKIN_FAIL NOT_SUPPORTED` (根據代碼，報到由硬體觸發，不接受軟體指令)。

-----

### 3\. 伺服器通知協定 (Server -\> Client)

伺服器會主動推送訊息給 Client (User)，通常發生在狀態改變或計時器觸發時。

| 通知類型 | 訊息格式 | 觸發時機/說明 |
| :--- | :--- | :--- |
| **輪到你了** | `INFO <id> YOUR_TURN 1 <seconds>` | 排隊輪到該使用者，需在 `<seconds>` 內刷卡報到。 |
| **報到成功** | `CHECKIN_OK <id> START_USE <seconds>` | 使用者刷卡成功，開始使用器材 `<seconds>` 秒。 |
| **詢問續用** | `MSG_CONTINUE_PROMPT <id> <seconds>` | 使用時間結束，無人排隊，詢問是否續用 (`<seconds>`秒內決定)。 |
| **續用確認** | `INFO <id> CONTINUE_YES <seconds>` | 使用者同意續用，需在 `<seconds>` 內再次刷卡。 |
| **不續用** | `INFO <id> CONTINUE_NO` | 使用者拒絕續用或逾時，釋放器材。 |
| **停權通知** | `INFO <id> BANNED <seconds>` | 因逾時未報到，被停權 `<seconds>` 秒。 |
| **重新排隊** | `INFO <id> REQUEUE_LAST` | 快速切換器材失敗（超時），被移至隊伍末端。 |

-----

### 4\. 內部硬體驅動協定 (Server \<-\> Equipment Driver)

這是 Server 與模擬硬體的 Driver 程式之間的通訊。Server 在此扮演 Client 角色連線至 Driver。

#### A. Server 發送給 Driver 的指令

| 指令 | 格式 | 說明 | 預期 Driver 回應 |
| :--- | :--- | :--- | :--- |
| **開始倒數** | `STARTCOUNTDOWN <sec> <uid> <phase>` | 要求硬體顯示倒數。<br>`phase`: `CHECKIN` 或 `USAGE`。 | `ACK 1` (成功)<br>`ACK 0` (失敗) |
| **停止倒數** | `STOPCOUNTDOWN` | 停止計時與顯示。 | `ACK 1` |
| **取得狀態** | `GETSTATUS` | 詢問硬體當前使用者與剩餘時間。 | `<user_id> <remain_sec>`<br>(若無人則回傳 `NONE 0`) |

#### B. Driver 發送給 Server 的事件 (Event)

Server 讀取 Driver 傳來的字串來觸發內部邏輯。

| 事件字串 | 說明 | Server 處理動作 |
| :--- | :--- | :--- |
| `USER_MISSED <user_id>` | 使用者未在時間內報到 | 觸發停權 (Ban) 或重新排隊邏輯。 |
| `USER_FINISHED <user_id>` | 使用者使用完畢 (硬體端判定) | 釋放器材，觸發「續用詢問」或通知下一位。 |
| `USER_CHECKEDIN <user_id>` | 使用者已刷卡報到 | 將狀態轉為 `EQUIP_IN_USE`，通知 Client `CHECKIN_OK`。 |

-----

### 5\. 狀態流轉示意 (State Flow Summary)

1.  **Reserve**: 使用者發送 `RESERVE` -\> 進入 `QUEUE` 或直接進入 `WAITING_CHECKIN`。
2.  **Wait**: 若在 `WAITING_CHECKIN`，Server 發送 `STARTCOUNTDOWN` 給 Driver。
      * **Case A (刷卡)**: Driver 發送 `USER_CHECKEDIN` -\> Server 轉態為 `IN_USE`，發送 `CHECKIN_OK` 給 User。
      * **Case B (逾時)**: Driver 發送 `USER_MISSED` -\> Server 轉態為 `IDLE`，發送 `BANNED` 給 User。
3.  **Use**: 進入 `IN_USE`，Server 監控時間。
      * **Time Up**: 時間到 -\> 觸發 `release_usage_and_promote`。
      * **Finish**: Driver 發送 `USER_FINISHED` -\> 觸發 `release_usage_and_promote`。
4.  **Promote**: 使用結束後：
      * 若有人排隊 -\> 通知下一位 (`YOUR_TURN`)。
      * 若無人排隊 -\> 發送 `PROMPT_CONTINUE` 詢問是否續用。

#!/usr/bin/env python3
import socket
import threading
import sys
import time

# ================= 設定區 =================
# 請將此 IP 改為 Raspberry Pi 的實際 IP，或保持 192.168.222.222
TARGET_IP = '192.168.222.222' 
TARGET_PORT = 8888

# ================= 接收執行緒 =================
def receive_handler(sock):
    """
    在背景持續監聽器材回傳的訊息
    包含回應 (ACK) 與 主動通知 (userFinished/userMissed)
    """
    buffer = ""
    try:
        while True:
            data = sock.recv(1024).decode('utf-8')
            if not data:
                print("\n[!] 連線已斷開 (Remote closed)")
                break
            
            buffer += data
            
            # 處理黏包 (Buffer Handling)
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                line = line.strip()
                if line:
                    # 使用不同顏色或前綴區分接收到的訊息
                    print(f"\n[<< Rx] 收到器材回應: {line}")
                    print("> ", end="", flush=True) # 保持輸入提示符號在最下方
                    
                    # 如果收到 userFinished 或 userMissed，模擬 Server 回傳 ACK
                    if line.startswith("userFinished") or line.startswith("userMissed"):
                        print(f"[Auto] 自動回覆 ACK 1 給器材...")
                        sock.sendall(b"ACK 1\n")

    except OSError:
        pass # Socket 關閉時會觸發，忽略
    except Exception as e:
        print(f"\n[!] 接收錯誤: {e}")

# ================= 主程式 =================
def main():
    print(f"正在連線至器材 {TARGET_IP}:{TARGET_PORT} ...")
    
    try:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.connect((TARGET_IP, TARGET_PORT))
        print("連線成功！")
    except Exception as e:
        print(f"連線失敗: {e}")
        return

    # 啟動背景接收執行緒
    recv_thread = threading.Thread(target=receive_handler, args=(client,), daemon=True)
    recv_thread.start()

    print("-" * 40)
    print("指令選單:")
    print(" 1. 查詢狀態 (getStatus)")
    print(" 2. 開始倒數 (startCountdown)")
    print(" 3. 停止倒數 (stopCountdown)")
    print(" q. 離開")
    print("-" * 40)

    try:
        while True:
            cmd_idx = input("> ").strip()
            
            msg_to_send = ""

            if cmd_idx == '1':
                msg_to_send = "getStatus"

            elif cmd_idx == '2':
                # 為了方便測試，這裡讓你可以輸入秒數和ID，預設 30秒 UserA
                try:
                    sec = input("  輸入倒數秒數 (預設 30): ").strip() or "30"
                    uid = input("  輸入 User ID (預設 UserA): ").strip() or "UserA"
                    msg_to_send = f"startCountdown {sec} {uid}"
                except ValueError:
                    print("輸入格式錯誤")
                    continue

            elif cmd_idx == '3':
                msg_to_send = "stopCountdown"

            elif cmd_idx.lower() == 'q':
                print("正在斷開連線...")
                break
            
            else:
                # 允許直接輸入原始指令測試 (例如手動打 getStatus)
                if cmd_idx:
                    msg_to_send = cmd_idx

            if msg_to_send:
                # 加上換行符號並發送
                full_cmd = msg_to_send + "\n"
                print(f"[>> Tx] 發送: {msg_to_send}")
                client.sendall(full_cmd.encode('utf-8'))
                
                # 稍微暫停讓 Rx 執行緒有機會印出回應
                time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n使用者中斷")
    finally:
        client.close()
        print("程式結束")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import socket
import threading
import time
import RPi.GPIO as GPIO
from mfrc522 import MFRC522
import tm1637

# ================= 設定區 =================
HOST = '0.0.0.0'
PORT = 8888
LED_PIN = 17      # BCM Pin 17

# 顯示器 GPIO (BCM)
CLK_PIN = 5
DIO_PIN = 6

# 狀態定義
STATE_IDLE = "IDLE"
STATE_CHECKIN = "CHECKIN"
STATE_COUNTDOWN = "COUNTDOWN"

# RFID 設定
CARD_TIMEOUT = 0.8
POLL_INTERVAL = 0.05

# ================= 全域變數與鎖 =================
class DeviceContext:
    def __init__(self):
        self.state = STATE_IDLE
        self.current_user = "NONE"
        self.remaining_time = 0
        self.lock = threading.Lock()
        self.stop_signal_received = False 
        self.msg_queue = [] 

ctx = DeviceContext()

# ================= RFID 偵測模組 =================
class RFIDReader:
    def __init__(self):
        self.reader = MFRC522()
        self.card_present = False
        self.last_seen_time = 0
        self.current_uid_str = None

    def poll(self):
        status, _ = self.reader.MFRC522_Request(self.reader.PICC_REQIDL)
        if status == self.reader.MI_OK:
            astatus, uid = self.reader.MFRC522_Anticoll()
            if astatus == self.reader.MI_OK:
                uid_str = "-".join(str(x) for x in uid)
                self.last_seen_time = time.time()
                if not self.card_present:
                    self.card_present = True
                    self.current_uid_str = uid_str
        
        if self.card_present:
            if time.time() - self.last_seen_time > CARD_TIMEOUT:
                self.card_present = False
                self.current_uid_str = None
        
        return self.card_present, self.current_uid_str

# ================= Socket 通訊執行緒 =================
def handle_client_connection(client_socket):
    buffer = ""
    client_socket.settimeout(0.1) 

    try:
        while True:
            try:
                data = client_socket.recv(1024).decode('utf-8')
                if not data: break
                
                buffer += data
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    line = line.strip()
                    if not line: continue
                    
                    response = parse_command(line)
                    if response:
                        client_socket.sendall((response + "\n").encode('utf-8'))
            
            except socket.timeout:
                pass
            except BlockingIOError:
                pass

            with ctx.lock:
                while ctx.msg_queue:
                    msg = ctx.msg_queue.pop(0)
                    print(f"[Socket] 主動發送: {msg}")
                    try:
                        client_socket.sendall((msg + "\n").encode('utf-8'))
                    except Exception as ex:
                        print(f"[Socket] 發送失敗: {ex}")

    except Exception as e:
        print(f"[Socket] 連線錯誤: {e}")
    finally:
        client_socket.close()
        print("[Socket] 連線中斷，等待重新連線...")

def parse_command(cmd_str):
    parts = cmd_str.split()
    cmd = parts[0]
    print(f"[Rx] 收到指令: {cmd_str}")

    if cmd == 'getStatus':
        with ctx.lock:
            if ctx.state == STATE_COUNTDOWN:
                return f"{ctx.current_user} {int(ctx.remaining_time)}"
            else:
                return f"NONE 0"

    elif cmd == 'startCountdown':
        if len(parts) < 3: return "ACK 0"
        try:
            sec = int(parts[1])
            u_id = parts[2]
            with ctx.lock:
                if ctx.state == STATE_IDLE:
                    ctx.remaining_time = sec
                    ctx.current_user = u_id
                    ctx.state = STATE_CHECKIN 
                    ctx.stop_signal_received = False
                    return "ACK 1"
                else:
                    return "ACK 0" 
        except ValueError:
            return "ACK 0"

    elif cmd == 'stopCountdown':
        with ctx.lock:
            ctx.stop_signal_received = True
            return "ACK 1"

    elif cmd == 'ACK':
        return
        
    return "ACK 0"

def socket_server_thread():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(1)
    print(f"Socket Server 啟動於 {HOST}:{PORT}")
    
    while True:
        client_sock, addr = server.accept()
        print(f"Server 已連線: {addr}")
        handle_client_connection(client_sock)

# ================= 硬體邏輯主迴圈 =================
def hardware_loop():
    # 1. GPIO 初始化
    GPIO.setwarnings(False)
    GPIO.setmode(GPIO.BCM)
    
    # 2. 初始化硬體
    rfid = RFIDReader()
    display = tm1637.TM1637(clk=CLK_PIN, dio=DIO_PIN, brightness=2)
    display.__del__ = lambda: None  # 防止自動清除時出錯
    display.write([0, 0, 0, 0])
    
    GPIO.setup(LED_PIN, GPIO.OUT)
    GPIO.output(LED_PIN, GPIO.LOW)
    
    checkin_start_time = 0
    last_loop_time = time.time()
    last_display_update = 0 

    print("硬體控制迴圈啟動...")

    try:
        while True:
            loop_start = time.time()
            dt = loop_start - last_loop_time
            last_loop_time = loop_start

            is_present, uid_str = rfid.poll()
            
            with ctx.lock:
                current_state = ctx.state
                stop_signal = ctx.stop_signal_received

            # --- 強制停止檢查 (Stop Countdown) ---
            if stop_signal and current_state != STATE_IDLE:
                print("[Logic] 收到強制停止訊號")
                
                # 若人還在，必須驅離並等待離開
                if is_present:
                    print("[Logic] 使用者仍滯留 -> 強制驅離模式 (等待離開)")
                    while True:
                        GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                        if int(time.time() * 5) % 2 == 0:
                            display.show("Stop")
                        else:
                            display.write([0, 0, 0, 0])
                        
                        time.sleep(0.2)
                        p, _ = rfid.poll()
                        if not p:
                            print("[Logic] 使用者已離開 -> 傳送 USERFINISHED")
                            break
                
                with ctx.lock:
                    ctx.msg_queue.append(f"userFinished {ctx.current_user}")
                    
                    ctx.state = STATE_IDLE
                    ctx.remaining_time = 0
                    ctx.current_user = "NONE"
                    ctx.stop_signal_received = False
                
                # [關鍵修正] 重置 Check-in 計時器
                checkin_start_time = 0 
                
                GPIO.output(LED_PIN, GPIO.LOW)
                display.write([0, 0, 0, 0])
                continue

            # --- STATE: IDLE (休息狀態 + 闖入偵測) ---
            if current_state == STATE_IDLE:
                GPIO.output(LED_PIN, GPIO.LOW)
                if time.time() - last_display_update > 1.0:
                    display.write([0, 0, 0, 0])
                    last_display_update = time.time()
                
                if is_present:
                    print("[Logic] 閒置中偵測到闖入 -> 閃燈驅離")
                    while True:
                        GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                        if int(time.time() * 5) % 2 == 0:
                            display.show("Err ")
                        else:
                            display.write([0, 0, 0, 0])
                        
                        time.sleep(0.2)
                        p, _ = rfid.poll()
                        if not p:
                            print("[Logic] 闖入者已離開 -> 回復閒置")
                            break
                    GPIO.output(LED_PIN, GPIO.LOW)
                    display.write([0, 0, 0, 0])

            # --- STATE: CHECKIN (等待報到) ---
            elif current_state == STATE_CHECKIN:
                # 若是第一次進入此狀態 (checkin_start_time 為 0)，則記錄當前時間
                if checkin_start_time == 0:
                    checkin_start_time = time.time()
                    print(f"[Logic] 等待 User: {ctx.current_user} 報到中...")
                
                elapsed = time.time() - checkin_start_time
                checkin_remain = 60 - elapsed
                
                if time.time() - last_display_update > 0.2:
                    if checkin_remain >= 0:
                        display.numbers(0, int(checkin_remain), colon=True)
                    last_display_update = time.time()
                
                if elapsed > 60:
                    print("[Logic] 逾時未報到 -> USERMISSED")
                    with ctx.lock:
                        ctx.msg_queue.append(f"userMissed {ctx.current_user}")
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                    checkin_start_time = 0 # 重置
                    display.write([0, 0, 0, 0])
                    continue

                if is_present:
                    if uid_str == ctx.current_user:
                        print("[Logic] 身分正確 -> 開始倒數")
                        with ctx.lock:
                            ctx.state = STATE_COUNTDOWN
                        checkin_start_time = 0 # 重置
                    else:
                        print(f"[Logic] 錯誤的人 ({uid_str}) -> 閃燈警告")
                        while True:
                            GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                            if int(time.time() * 5) % 2 == 0:
                                display.show("Err ")
                            else:
                                display.write([0,0,0,0])
                            time.sleep(0.2)
                            p, _ = rfid.poll()
                            if not p: break 
                        GPIO.output(LED_PIN, GPIO.LOW)

            # --- STATE: COUNTDOWN (倒數狀態) ---
            elif current_state == STATE_COUNTDOWN:
                GPIO.output(LED_PIN, GPIO.HIGH)

                with ctx.lock:
                    ctx.remaining_time -= dt
                
                rem_time = int(ctx.remaining_time)
                
                if rem_time >= 0:
                    mins = rem_time // 60
                    secs = rem_time % 60
                    display.numbers(mins, secs, colon=True) 

                if int(ctx.remaining_time) % 5 == 0 and int(ctx.remaining_time) != int(ctx.remaining_time + dt):
                     print(f"[Logic] 剩餘時間: {rem_time}")

                time_up = ctx.remaining_time <= 0
                person_left = not is_present
                
                if time_up or person_left:
                    reason = "時間到" if time_up else "人離開"
                    print(f"[Logic] 偵測到結束條件 ({reason})")

                    if is_present:
                        print("[Logic] 但使用者仍滯留 -> 進入驅離模式 (等待離開)")
                        while True:
                            GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                            if int(time.time() * 5) % 2 == 0:
                                display.show("End ")
                            else:
                                display.write([0, 0, 0, 0])
                            
                            time.sleep(0.2)
                            p, _ = rfid.poll()
                            if not p:
                                print("[Logic] 使用者已離開 -> 準備發送訊號")
                                break

                    with ctx.lock:
                        target_user = ctx.current_user 
                        ctx.msg_queue.append(f"userFinished {target_user}")
                        
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                        ctx.remaining_time = 0
                    
                    GPIO.output(LED_PIN, GPIO.LOW)
                    display.write([0, 0, 0, 0])

            time.sleep(POLL_INTERVAL)

    except KeyboardInterrupt:
        print("程式終止")

    finally:
        print("正在關閉硬體...")
        
        # --- 終極修正 Start ---
        try:
            # 1. 再次強制設定模式 (這是關鍵!)
            # 這樣做是為了防止 GPIO 模式在前面意外跑掉，
            # 確保接下來 tm1637 的 __del__ 執行時，GPIO 模式一定是正確的 BCM。
            GPIO.setmode(GPIO.BCM)
            
            # 2. 安全地關閉顯示器
            if 'display' in locals():
                try:
                    display.write([0, 0, 0, 0]) # 嘗試清空畫面
                    display.brightness(0)
                except:
                    pass
                
                # 3. 手動銷毀物件
                # 因為上面已經強制 setmode 了，這裡觸發 __del__ 就不會報錯
                del display
                
        except Exception as e:
            # 這裡捕捉所有錯誤，確保不會影響最後的 cleanup
            print(f"顯示器關閉例外 (已忽略): {e}")
        # --- 終極修正 End ---

        # 4. 最後釋放所有 GPIO 資源
        try:
            GPIO.cleanup()
        except:
            pass
            
        print("硬體資源已釋放")

if __name__ == "__main__":
    net_t = threading.Thread(target=socket_server_thread, daemon=True)
    net_t.start()
    hardware_loop()

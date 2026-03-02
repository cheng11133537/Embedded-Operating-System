#!/usr/bin/env python3
import logging
import socket
import threading
import time
import RPi.GPIO as GPIO
from mfrc522 import MFRC522
import tm1637

# 協定常數：請在同一個資料夾放 protocol_common.py
from protocol_common import (
    CMD_GETSTATUS,
    CMD_STARTCOUNTDOWN,
    CMD_STOPCOUNTDOWN,
    EVT_USER_CHECKEDIN,
    EVT_USER_FINISHED,
    EVT_USER_MISSED,
)

logger = logging.getLogger("Device")
handler = logging.StreamHandler()
formatter = logging.Formatter(
    "[%(asctime)s][%(levelname)s][Device] %(message)s",
    "%Y-%m-%d %H:%M:%S",
)
handler.setFormatter(formatter)
logger.addHandler(handler)
logger.setLevel(logging.INFO)

USER_RFID_BINDINGS = {
    "U1": "193-86-91-6-202",
    "U2": "220-91-52-6-181",
    "U3": "51-131-14-6-184",
    "U4": "136-4-112-61-193"
}

def uid_matches(uid_str, target):
    if not uid_str or not target: return False
    if uid_str == target: return True
    if uid_str.replace("-", ".") == target: return True
    if target.replace(".", "-") == uid_str: return True
    return False

# ================= 設定區 =================
HOST = "0.0.0.0"
PORT = 9999
LED_PIN = 19
BUZZER_PIN = 26  # 蜂鳴器控制腳位

# 顯示器 GPIO
CLK_PIN = 5
DIO_PIN = 6

STATE_IDLE = "IDLE"
STATE_CHECKIN = "CHECKIN"
STATE_COUNTDOWN = "COUNTDOWN"

CARD_TIMEOUT = 0.8
POLL_INTERVAL = 0.05

class DeviceContext:
    def __init__(self):
        self.state = STATE_IDLE
        self.current_user = "NONE"
        self.remaining_time = 0
        self.lock = threading.Lock()
        self.stop_signal_received = False
        self.msg_queue = []
        self.checkin_verified = False

ctx = DeviceContext()

# ================= 蜂鳴器控制 (PWM) =================
# 全域變數存放 PWM 實例
buzzer_pwm = None

def buzzer_init():
    global buzzer_pwm
    GPIO.setup(BUZZER_PIN, GPIO.OUT)
    # HS-1203A 聲音頻率約在 2000Hz - 2700Hz 之間，這裡設 2500Hz
    buzzer_pwm = GPIO.PWM(BUZZER_PIN, 2500) 

def buzzer_on():
    """開啟蜂鳴器聲音"""
    global buzzer_pwm
    # 佔空比 50% 聲音最大
    buzzer_pwm.start(50)

def buzzer_off():
    """關閉蜂鳴器聲音"""
    global buzzer_pwm
    buzzer_pwm.stop()

# ================= RFID 模組 =================
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

# ================= Socket 通訊 =================
def handle_client_connection(client_socket):
    buffer = ""
    client_socket.settimeout(0.1)
    try:
        while True:
            try:
                data = client_socket.recv(1024).decode("utf-8")
                if not data: break
                buffer += data
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line: continue
                    response = parse_command(line)
                    if response:
                        client_socket.sendall((response + "\n").encode("utf-8"))
            except socket.timeout: pass
            except BlockingIOError: pass

            with ctx.lock:
                while ctx.msg_queue:
                    msg = ctx.msg_queue.pop(0)
                    logger.info("[Socket] 主動發送: %s", msg)
                    try: client_socket.sendall((msg + "\n").encode("utf-8"))
                    except Exception as ex: logger.warning("[Socket] 發送失敗: %s", ex)
    except Exception as e: logger.error("[Socket] 連線錯誤: %s", e)
    finally:
        client_socket.close()
        logger.info("[Socket] 連線中斷，等待重新連線...")

def parse_command(cmd_str):
    parts = cmd_str.split()
    if not parts: return None
    cmd = parts[0].upper()
    
    if cmd == CMD_GETSTATUS:
        with ctx.lock:
            if ctx.state == STATE_COUNTDOWN: return f"{ctx.current_user} {int(ctx.remaining_time)}"
            if ctx.state == STATE_CHECKIN and ctx.checkin_verified: return f"{ctx.current_user} 0"
            return "NONE 0"

    elif cmd == CMD_STARTCOUNTDOWN:
        if len(parts) < 2: return "ACK 0"
        try:
            sec = int(parts[1])
            u_id = parts[2] if len(parts) >= 3 else "NONE"
            phase = parts[3].upper() if len(parts) >= 4 else "CHECKIN"
            with ctx.lock:
                if phase == "CHECKIN":
                    ctx.remaining_time = sec
                    ctx.current_user = u_id
                    ctx.state = STATE_CHECKIN
                    ctx.checkin_verified = False
                    ctx.stop_signal_received = False
                    return "ACK 1"
                elif phase == "USAGE":
                    if ctx.state == STATE_CHECKIN and ctx.checkin_verified:
                        ctx.remaining_time = sec
                        ctx.state = STATE_COUNTDOWN
                        ctx.checkin_verified = False
                        ctx.stop_signal_received = False
                        return "ACK 1"
                    return "ACK 0"
                else: return "ACK 0"
        except ValueError: return "ACK 0"

    elif cmd == CMD_STOPCOUNTDOWN:
        with ctx.lock:
            ctx.stop_signal_received = True
            return "ACK 1"
    elif cmd == "ACK": return None
    return "ACK 0"

def socket_server_thread():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(1)
    logger.info("Socket Server 啟動於 %s:%d", HOST, PORT)
    while True:
        client_sock, addr = server.accept()
        logger.info("Server 已連線: %s", addr)
        handle_client_connection(client_sock)

# ================= 硬體邏輯主迴圈 =================
def hardware_loop():
    GPIO.setwarnings(False)
    GPIO.setmode(GPIO.BCM)

    rfid = RFIDReader()
    display = tm1637.TM1637(clk=CLK_PIN, dio=DIO_PIN, brightness=2)
    display.__del__ = lambda: None
    display.write([0, 0, 0, 0])

    GPIO.setup(LED_PIN, GPIO.OUT)
    GPIO.output(LED_PIN, GPIO.LOW)
    
    # [UPDATE] 初始化 PWM 蜂鳴器
    buzzer_init()
    buzzer_off()

    checkin_start_time = 0
    last_loop_time = time.time()
    last_display_update = 0

    logger.info("硬體控制迴圈啟動...")

    try:
        while True:
            loop_start = time.time()
            dt = loop_start - last_loop_time
            last_loop_time = loop_start

            is_present, uid_str = rfid.poll()

            with ctx.lock:
                current_state = ctx.state
                stop_signal = ctx.stop_signal_received

            # --- 強制停止檢查 ---
            if stop_signal and current_state != STATE_IDLE:
                logger.info("[Logic] 收到強制停止訊號")
                with ctx.lock: already_checked_in = ctx.checkin_verified
                if is_present and already_checked_in: continue

                if is_present:
                    logger.info("[Logic] 強制驅離模式")
                    while True:
                        # [UPDATE] 閃爍邏輯：LED亮+蜂鳴器叫 -> LED滅+蜂鳴器停
                        GPIO.output(LED_PIN, GPIO.HIGH)
                        buzzer_on()  # 叫
                        
                        if int(time.time() * 5) % 2 == 0: display.show("Stop")
                        else: display.write([0, 0, 0, 0])
                        
                        time.sleep(0.2)
                        
                        GPIO.output(LED_PIN, GPIO.LOW)
                        buzzer_off() # 停
                        
                        # 再停頓一點時間，形成斷續感
                        time.sleep(0.2)

                        p, _ = rfid.poll()
                        if not p:
                            break

                with ctx.lock:
                    ctx.state = STATE_IDLE
                    ctx.remaining_time = 0
                    ctx.current_user = "NONE"
                    ctx.stop_signal_received = False
                    ctx.checkin_verified = False

                checkin_start_time = 0
                GPIO.output(LED_PIN, GPIO.LOW)
                buzzer_off()
                display.write([0, 0, 0, 0])
                continue

            # --- STATE: IDLE ---
            if current_state == STATE_IDLE:
                GPIO.output(LED_PIN, GPIO.LOW)
                buzzer_off() # 確保安靜

                if time.time() - last_display_update > 1.0:
                    display.write([0, 0, 0, 0])
                    last_display_update = time.time()

                if is_present:
                    logger.info("[Logic] 闖入偵測 -> 警報")
                    while True:
                        # [UPDATE] 警報閃爍
                        GPIO.output(LED_PIN, GPIO.HIGH)
                        buzzer_on()
                        
                        if int(time.time() * 5) % 2 == 0: display.show("Err ")
                        else: display.write([0, 0, 0, 0])
                        
                        time.sleep(0.2)
                        
                        GPIO.output(LED_PIN, GPIO.LOW)
                        buzzer_off()
                        time.sleep(0.2)

                        p, _ = rfid.poll()
                        if not p: break
                    
                    GPIO.output(LED_PIN, GPIO.LOW)
                    buzzer_off()
                    display.write([0, 0, 0, 0])

            # --- STATE: CHECKIN ---
            elif current_state == STATE_CHECKIN:
                if checkin_start_time == 0:
                    checkin_start_time = time.time()
                    logger.info("[Logic] 等待 User: %s 報到...", ctx.current_user)

                with ctx.lock:
                    ctx.remaining_time -= dt
                    if ctx.remaining_time < 0: ctx.remaining_time = 0
                    checkin_remain = int(ctx.remaining_time)

                if time.time() - last_display_update > 0.2:
                    if checkin_remain >= 0: display.numbers(0, checkin_remain, colon=True)
                    last_display_update = time.time()

                if checkin_remain <= 0:
                    logger.warning("[Logic] 逾時未報到")
                    with ctx.lock:
                        ctx.msg_queue.append(f"{EVT_USER_MISSED} {ctx.current_user}")
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                        ctx.checkin_verified = False
                        ctx.remaining_time = 0
                    checkin_start_time = 0
                    display.write([0, 0, 0, 0])
                    continue

                if is_present:
                    target_uid = USER_RFID_BINDINGS.get(ctx.current_user)
                    if target_uid is None:
                        logger.warning("[Logic] 無綁定卡號")
                    else:
                        if uid_matches(uid_str, target_uid):
                            logger.info("[Logic] 報到成功")
                            with ctx.lock:
                                ctx.checkin_verified = True
                                ctx.msg_queue.append(f"{EVT_USER_CHECKEDIN} {ctx.current_user}")
                            checkin_start_time = 0
                        else:
                            logger.warning("[Logic] 錯誤的人 -> 警告")
                            while True:
                                # [UPDATE] 錯誤警告
                                GPIO.output(LED_PIN, GPIO.HIGH)
                                buzzer_on()
                                
                                if int(time.time() * 5) % 2 == 0: display.show("Err ")
                                else: display.write([0, 0, 0, 0])
                                
                                time.sleep(0.2)
                                
                                GPIO.output(LED_PIN, GPIO.LOW)
                                buzzer_off()
                                time.sleep(0.2)

                                p, _ = rfid.poll()
                                if not p: break
                            GPIO.output(LED_PIN, GPIO.LOW)
                            buzzer_off()

            # --- STATE: COUNTDOWN ---
            elif current_state == STATE_COUNTDOWN:
                GPIO.output(LED_PIN, GPIO.HIGH)
                buzzer_off() # 使用中安靜

                with ctx.lock:
                    ctx.remaining_time -= dt
                    rem_time = int(ctx.remaining_time)

                if rem_time >= 0:
                    mins = rem_time // 60
                    secs = rem_time % 60
                    display.numbers(mins, secs, colon=True)

                if int(ctx.remaining_time) % 5 == 0 and int(ctx.remaining_time) != int(ctx.remaining_time + dt):
                    logger.info("[Logic] 剩餘時間: %d", rem_time)

                time_up = ctx.remaining_time <= 0
                person_left = not is_present

                if time_up or person_left:
                    reason = "時間到" if time_up else "人離開"
                    logger.info("[Logic] 結束: %s", reason)

                    if is_present:
                        logger.info("[Logic] 驅離模式")
                        while True:
                            # [UPDATE] 結束驅離
                            GPIO.output(LED_PIN, GPIO.HIGH)
                            buzzer_on()
                            
                            if int(time.time() * 5) % 2 == 0: display.show("End ")
                            else: display.write([0, 0, 0, 0])
                            
                            time.sleep(0.2)
                            
                            GPIO.output(LED_PIN, GPIO.LOW)
                            buzzer_off()
                            time.sleep(0.2)

                            p, _ = rfid.poll()
                            if not p: break

                    with ctx.lock:
                        target_user = ctx.current_user
                        ctx.msg_queue.append(f"{EVT_USER_FINISHED} {target_user}")
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                        ctx.remaining_time = 0
                        ctx.checkin_verified = False

                    GPIO.output(LED_PIN, GPIO.LOW)
                    buzzer_off()
                    display.write([0, 0, 0, 0])

            time.sleep(POLL_INTERVAL)

    except KeyboardInterrupt:
        logger.info("程式終止")

    finally:
        logger.info("正在關閉硬體...")
        # [UPDATE] 停止 PWM
        try:
            if buzzer_pwm:
                buzzer_pwm.stop()
        except: pass
        
        try:
            GPIO.setmode(GPIO.BCM)
            if "display" in locals():
                try: display.write([0, 0, 0, 0]); display.brightness(0)
                except: pass
                del display
        except: pass

        try: GPIO.cleanup()
        except: pass
        logger.info("硬體資源已釋放")

if __name__ == "__main__":
    net_t = threading.Thread(target=socket_server_thread, daemon=True)
    net_t.start()
    hardware_loop()

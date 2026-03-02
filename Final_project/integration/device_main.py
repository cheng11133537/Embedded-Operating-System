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

# User RFID bindings: server user_id -> physical card UID
USER_RFID_BINDINGS = {
    "U1": "193-86-91-6-202",
    "U2": "220-91-52-6-181",
    "U3": "51-131-14-6-184"
}

def uid_matches(uid_str, target):
    """比較實際讀到的 uid_str 和綁定的 target 是否相同，允許 '-' / '.' 差異。"""
    if not uid_str or not target:
        return False
    if uid_str == target:
        return True
    if uid_str.replace("-", ".") == target:
        return True
    if target.replace(".", "-") == uid_str:
        return True
    return False

# ================= 設定區 =================
HOST = "0.0.0.0"
PORT = 9999
LED_PIN = 17  # BCM Pin 17

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
        self.msg_queue = []  # 要送回 server 的事件
        self.checkin_verified = False


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
                data = client_socket.recv(1024).decode("utf-8")
                if not data:
                    break

                buffer += data
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    response = parse_command(line)
                    if response:
                        client_socket.sendall((response + "\n").encode("utf-8"))

            except socket.timeout:
                pass
            except BlockingIOError:
                pass

            # 主動事件發送（userFinished / userMissed）
            with ctx.lock:
                while ctx.msg_queue:
                    msg = ctx.msg_queue.pop(0)
                    logger.info("[Socket] 主動發送: %s", msg)
                    try:
                        client_socket.sendall((msg + "\n").encode("utf-8"))
                    except Exception as ex:
                        logger.warning("[Socket] 發送失敗: %s", ex)

    except Exception as e:
        logger.error("[Socket] 連線錯誤: %s", e)
    finally:
        client_socket.close()
        logger.info("[Socket] 連線中斷，等待重新連線...")


def parse_command(cmd_str):
    parts = cmd_str.split()
    if not parts:
        return None
    cmd = parts[0].upper()
    logger.debug("[Rx] 收到指令: %s", cmd_str)

    # GETSTATUS：回傳當前 user + 剩餘秒數
    if cmd == CMD_GETSTATUS:
        with ctx.lock:
            if ctx.state == STATE_COUNTDOWN:
                return f"{ctx.current_user} {int(ctx.remaining_time)}"
            if ctx.state == STATE_CHECKIN and ctx.checkin_verified:
                return f"{ctx.current_user} 0"
            return "NONE 0"

    # STARTCOUNTDOWN <sec> [user]
    elif cmd == CMD_STARTCOUNTDOWN:
        if len(parts) < 2:
            return "ACK 0"
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
                else:
                    return "ACK 0"
        except ValueError:
            return "ACK 0"

    # STOPCOUNTDOWN
    elif cmd == CMD_STOPCOUNTDOWN:
        with ctx.lock:
            ctx.stop_signal_received = True
            return "ACK 1"

    # ACK（server 可能回 ACK，我們目前不用處理）
    elif cmd == "ACK":
        return None

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
    # 1. GPIO 初始化
    GPIO.setwarnings(False)
    GPIO.setmode(GPIO.BCM)

    # 2. 初始化硬體
    rfid = RFIDReader()
    display = tm1637.TM1637(clk=CLK_PIN, dio=DIO_PIN, brightness=2)
    # 防止 tm1637 內建 __del__ 在錯誤時炸掉
    display.__del__ = lambda: None
    display.write([0, 0, 0, 0])

    GPIO.setup(LED_PIN, GPIO.OUT)
    GPIO.output(LED_PIN, GPIO.LOW)

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

            # --- 強制停止檢查 (Stop Countdown) ---
            if stop_signal and current_state != STATE_IDLE:
                logger.info("[Logic] 收到強制停止訊號")

                # 若人還在，必須驅離並等待離開
                with ctx.lock:
                    already_checked_in = ctx.checkin_verified
                if is_present and already_checked_in:
                    continue

                if is_present:
                    logger.info("[Logic] 使用者仍滯留 -> 強制驅離模式 (等待離開)")
                    while True:
                        GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                        if int(time.time() * 5) % 2 == 0:
                            display.show("Stop")
                        else:
                            display.write([0, 0, 0, 0])

                        time.sleep(0.2)
                        p, _ = rfid.poll()
                        if not p:
                            logger.info("[Logic] 使用者已離開 -> 傳送 USERFINISHED")
                            break

                with ctx.lock:
                    # ctx.msg_queue.append(f"{EVT_USER_FINISHED} {ctx.current_user}")
                    ctx.state = STATE_IDLE
                    ctx.remaining_time = 0
                    ctx.current_user = "NONE"
                    ctx.stop_signal_received = False
                    ctx.checkin_verified = False

                # 重置 Check-in 計時器
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
                    logger.info("[Logic] 閒置中偵測到闖入 -> 閃燈驅離")
                    while True:
                        GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                        if int(time.time() * 5) % 2 == 0:
                            display.show("Err ")
                        else:
                            display.write([0, 0, 0, 0])

                        time.sleep(0.2)
                        p, _ = rfid.poll()
                        if not p:
                            logger.info("[Logic] 闖入者已離開 -> 回復閒置")
                            break
                    GPIO.output(LED_PIN, GPIO.LOW)
                    display.write([0, 0, 0, 0])

            # --- STATE: CHECKIN (等待報到) ---
            elif current_state == STATE_CHECKIN:
                # 若是第一次進入此狀態 (checkin_start_time 為 0)，則記錄當前時間
                if checkin_start_time == 0:
                    checkin_start_time = time.time()
                    logger.info("[Logic] 等待 User: %s 報到中...", ctx.current_user)

                with ctx.lock:
                    ctx.remaining_time -= dt
                    if ctx.remaining_time < 0:
                        ctx.remaining_time = 0
                    checkin_remain = int(ctx.remaining_time)

                if time.time() - last_display_update > 0.2:
                    if checkin_remain >= 0:
                        display.numbers(0, checkin_remain, colon=True)
                    last_display_update = time.time()

                # 報到逾時
                if checkin_remain <= 0:
                    logger.warning("[Logic] 逾時未報到 -> USERMISSED")
                    with ctx.lock:
                        ctx.msg_queue.append(f"{EVT_USER_MISSED} {ctx.current_user}")
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                        ctx.checkin_verified = False
                        ctx.remaining_time = 0

                    checkin_start_time = 0
                    display.write([0, 0, 0, 0])
                    continue

                # 卡片偵測（這裡改成用 USER_RFID_BINDINGS + uid_matches）
                if is_present:
                    # 根據目前輪到的 user_id 查出綁定卡號
                    target_uid = USER_RFID_BINDINGS.get(ctx.current_user)

                    # 沒綁卡的情況：先拒絕報到（也可以改成接受任意卡）
                    if target_uid is None:
                        logger.warning(
                            "[Logic] 使用者 %s 沒有綁定卡號，拒絕報到 (卡號=%s)",
                            ctx.current_user,
                            uid_str,
                        )
                    else:
                        # 比對卡號是否符合（支援 '-' / '.' 差異）
                        if uid_matches(uid_str, target_uid):
                            logger.info(
                                "[Logic] 身分正確 -> 報到成功 (user=%s, card=%s)",
                                ctx.current_user,
                                uid_str,
                            )
                            with ctx.lock:
                                ctx.checkin_verified = True
                                ctx.msg_queue.append(
                                    f"{EVT_USER_CHECKEDIN} {ctx.current_user}"
                                )
                            checkin_start_time = 0
                        else:
                            logger.warning(
                                "[Logic] 錯誤的人 (讀到卡=%s, 期待卡=%s) -> 閃燈警告",
                                uid_str,
                                target_uid,
                            )
                            while True:
                                GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                                if int(time.time() * 5) % 2 == 0:
                                    display.show("Err ")
                                else:
                                    display.write([0, 0, 0, 0])
                                time.sleep(0.2)
                                p, _ = rfid.poll()
                                if not p:
                                    break
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

                # 每 5 秒 log 一次
                if (
                    int(ctx.remaining_time) % 5 == 0
                    and int(ctx.remaining_time) != int(ctx.remaining_time + dt)
                ):
                    logger.info("[Logic] 剩餘時間: %d", rem_time)

                time_up = ctx.remaining_time <= 0
                person_left = not is_present

                if time_up or person_left:
                    reason = "時間到" if time_up else "人離開"
                    logger.info("[Logic] 偵測到結束條件 (%s)", reason)

                    if is_present:
                        logger.info(
                            "[Logic] 但使用者仍滯留 -> 進入驅離模式 (等待離開)"
                        )
                        while True:
                            GPIO.output(LED_PIN, not GPIO.input(LED_PIN))
                            if int(time.time() * 5) % 2 == 0:
                                display.show("End ")
                            else:
                                display.write([0, 0, 0, 0])

                            time.sleep(0.2)
                            p, _ = rfid.poll()
                            if not p:
                                logger.info("[Logic] 使用者已離開 -> 準備發送訊號")
                                break

                    with ctx.lock:
                        target_user = ctx.current_user
                        ctx.msg_queue.append(
                            f"{EVT_USER_FINISHED} {target_user}"
                        )
                        ctx.state = STATE_IDLE
                        ctx.current_user = "NONE"
                        ctx.remaining_time = 0
                        ctx.checkin_verified = False

                    GPIO.output(LED_PIN, GPIO.LOW)
                    display.write([0, 0, 0, 0])

            time.sleep(POLL_INTERVAL)

    except KeyboardInterrupt:
        logger.info("程式終止")

    finally:
        logger.info("正在關閉硬體...")

        # --- 終極修正 Start ---
        try:
            # 再次強制設定模式，避免 tm1637 __del__ 出錯
            GPIO.setmode(GPIO.BCM)

            if "display" in locals():
                try:
                    display.write([0, 0, 0, 0])
                    display.brightness(0)
                except Exception:
                    pass
                del display

        except Exception as e:
            logger.warning("顯示器關閉例外 (已忽略): %s", e)
        # --- 終極修正 End ---

        try:
            GPIO.cleanup()
        except Exception:
            pass

        logger.info("硬體資源已釋放")


if __name__ == "__main__":
    net_t = threading.Thread(target=socket_server_thread, daemon=True)
    net_t.start()
    hardware_loop()

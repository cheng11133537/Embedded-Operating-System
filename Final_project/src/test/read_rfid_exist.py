#!/usr/bin/env python3
import RPi.GPIO as GPIO
from mfrc522 import MFRC522
import time

reader = MFRC522()

CARD_TIMEOUT = 0.8   # 超過 0.8 秒沒讀到就算卡離開
POLL_INTERVAL = 0.05 # 每 50ms 檢查一次

card_present = False
last_seen_time = 0
current_uid = None

def uid_to_str(uid):
    return "-".join(str(x) for x in uid)

print("開始 RFID 偵測（Ctrl+C 結束）")

try:
    while True:
        # 嘗試偵測卡
        status, _ = reader.MFRC522_Request(reader.PICC_REQIDL)

        if status == reader.MI_OK:
            # 嘗試讀UID
            astatus, uid = reader.MFRC522_Anticoll()
            if astatus == reader.MI_OK:
                uid_str = uid_to_str(uid)

                # 更新「最後看到卡片」時間
                last_seen_time = time.time()

                if not card_present:
                    card_present = True
                    current_uid = uid_str
                    print(f"👉 卡片靠近：{current_uid}")

        # 檢查是否超過 timeout 沒看到卡片
        if card_present:
            if time.time() - last_seen_time > CARD_TIMEOUT:
                print(f"👋 卡片離開：{current_uid}")
                card_present = False
                current_uid = None

        time.sleep(POLL_INTERVAL)

except KeyboardInterrupt:
    print("結束")

finally:
    GPIO.cleanup()

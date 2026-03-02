#!/usr/bin/env python3
import RPi.GPIO as GPIO
from mfrc522 import SimpleMFRC522
import time

reader = SimpleMFRC522()

print("RFID 持續讀取中...")
print("按 Ctrl+C 結束程式")

try:
    while True:
        print("等待卡片中...")
        id, text = reader.read()  # 會卡住直到讀到卡片
        
        print("---------------")
        print(f"卡片 ID: {id}")
        print(f"卡片內容: {text}")
        print("---------------")

        time.sleep(0.5)  # 可避免連續重複讀到同一張卡（建議加）

except KeyboardInterrupt:
    print("\n程式停止")

finally:
    GPIO.cleanup()

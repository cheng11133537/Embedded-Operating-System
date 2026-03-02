#!/usr/bin/env python3
import RPi.GPIO as GPIO
from mfrc522 import SimpleMFRC522

# 建立閱讀器物件
reader = SimpleMFRC522()

print("請將卡片或磁扣靠近 RC522 讀卡機...")
print("按 Ctrl+C 結束程式")

try:
    # 讀取卡片 (此函數會暫停程式直到讀到卡片)
    id, text = reader.read()
    print(f"讀取成功!")
    print(f"卡片 ID (UID): {id}")
    print(f"卡片內容 (Text): {text}")

except KeyboardInterrupt:
    print("\n程式已停止")

finally:
    GPIO.cleanup()

import time
import tm1637

# 設定 GPIO 腳位 (使用 BCM 編號)
CLK_PIN = 5
DIO_PIN = 6

# 初始化顯示器
# brightness 範圍是 0 (最暗) 到 7 (最亮)
display = tm1637.TM1637(clk=CLK_PIN, dio=DIO_PIN, brightness=2)

def main():
    print("開始測試 Grove 4-Digit Display...")
    
    try:
        while True:
            # 1. 顯示數字 (0-9999 計數測試)
            print("測試計數...")
            for i in range(20): # 改短一點方便測試
                display.number(i)
                time.sleep(0.1)
            
            # --- 修正點：清除螢幕 ---
            # write([0,0,0,0]) 代表傳送 4 個「全滅」的訊號給顯示器
            display.write([0, 0, 0, 0]) 
            time.sleep(0.5)

            # 2. 顯示時間格式 (例如 12:30)
            print("測試時間顯示 (冒號閃爍)...")
            display.numbers(12, 30, colon=True) 
            time.sleep(1)
            display.numbers(12, 30, colon=False)
            time.sleep(1)
            
            # 3. 調整亮度測試
            print("測試亮度變化...")
            display.numbers(88, 88, colon=True)
            for b in range(0, 8):
                display.brightness(b)
                time.sleep(0.2)
            
            # 恢復亮度
            display.brightness(2)
            
            # 4. 顯示文字 (TM1637 只能顯示部分字母)
            print("顯示文字 (COOL)...")
            display.show("COOL")
            time.sleep(2)

            # --- 修正點：清除螢幕 ---
            display.write([0, 0, 0, 0])
            time.sleep(1)

    except KeyboardInterrupt:
        # 當按下 Ctrl+C 時，清除螢幕並退出
        print("\n程式結束")
        # --- 修正點：清除螢幕 ---
        display.write([0, 0, 0, 0])

if __name__ == "__main__":
    main()

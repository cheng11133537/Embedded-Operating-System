# 📁 Towel_Detector (AI 辨識模組)

使用 YOLOv8 進行毛巾偵測

## 專案結構
```text
.
├── Towel_Detector_Server.py  # [核心] Python WebSocket Server，負責接收影像與回傳結果
├── best.pt                   # [模型] 訓練好的 YOLOv8 權重檔
├── requirements.txt          # [環境] Server 執行所需的 Python 套件
├── Towel_client.c            # [測試] 用來測試 Server 連線的 Client，要改成 Linux 編譯得微調
└── README.md                 # 使用說明
```

## 🚀 如何啟動 Server

1. **安裝環境**
   ```bash
   pip install -r requirements.txt
   
2. **開啟 Server**
   ```bash
   # best.pt 請放在與 Towel_Detector_Server.py 同一個資料夾
   
   python Towel_Detector_Server.py
   
   # Server 將在 port 9000 等待連線
   
3. **編出exe檔**
   ```bash
   # 在「MinGW / gcc 的終端機」裡編出 Towel_client.c 的執行檔
   
   gcc Towel_client.c -o Towel_client.exe -lws2_32
 
   
4. **開啟 Client**
   ```bash
   # 格式: <執行檔> <Server_IP> <Port>
   # 請透過 ipconfig (Windows) 或 ifconfig (Linux) 查看 Server 的 IPv4 address
   
   Towel_client.exe 192.168.x.x 9000
   
   # 連上後可以點開 Server 跳出的辨識畫面
   
## 📋 說明
1.  透過 TCP 回傳有無毛巾 (AI 建議如果連同一個網路熱點不需要 http)
2.  有三種條件比較容易偵測成功，分別是手拿著毛巾自然垂放在身體側邊、把毛巾打開水平展示在胸口、把毛巾整陀拿著靠近鏡頭
3.  辨識時可在 client 端加入累積收到幾次 HasTowel 或是要連續幾秒 HasTowel 才算是可以入場

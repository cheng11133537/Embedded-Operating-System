---

# 📡 RPI（裝置端）與 Server 連線操作流程整理

（所有程式碼位於 GitHub：`EOS2025F_group5/src/test/`）

---

## ⭐ **1. 讓 Raspberry Pi 連上網路**

在 RPI 終端機輸入：

```bash
sudo nmcli dev wifi connect "網路名稱" password "網路密碼"
```

成功後，查看 RPI 的 IP：

```bash
ifconfig
```

➡ **記下顯示的 IP（server 之後要連到這個 IP）。**

---

## ⭐ **2. 啟動 RPI 的 device 程式**

### (1) 把 `device_main.py` 放進 RPI 的 `FP/` 資料夾

（確保路徑正確，例如：`~/FP/device_main.py`）

### (2) 進入資料夾

```bash
cd FP
```

### (3) 啟動 Python 虛擬環境

```bash
source .venv/bin/activate
```
啟動完後左邊應該會有(.venv)表示在虛擬環境裡了, 接下來可以在虛擬環境 pip install 或是啟動 python code。

### (4) 執行裝置程式

`device_main.py` 裡的 port 預設寫死為 **8888**（可自行修改）。

```bash
python device_main.py
```

➡ 這樣 **device 端 server 就啟動成功**，等待外部連線。

---

## ⭐ **3. Server 端如何連線？**

Server 端連線時，只要連到：

* **IP：剛剛 `ifconfig` 查到的 RPI IP**
* **Port：8888**

範例（依你的連線方式不同而變化）：

```python
import socket

s = socket.socket()
s.connect(("RPI_IP_在這裡", 8888))
```

➡ 連上後即可與 RPI 的裝置程式通訊。

---

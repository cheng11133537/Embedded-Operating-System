from ultralytics import YOLO
import cv2
import socket
import os


# 手拿著毛巾自然垂放在身體側邊
# 把毛巾打開水平展示在胸口
# 把毛巾整陀拿著靠近鏡頭

# 自動抓跟這支檔案同一個資料夾裡的 best.pt
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(BASE_DIR, "best.pt")

# 0: towel, 1: bag -> 這兩類都當成有帶毛巾
TOWEL_CLASS_ID = 0
BAG_CLASS_ID = 1
CONF_TH = 0.5

# ROI 佔整個畫面的比例（保留效果，但沒畫出來，可自行更改）
ROI_X1_FRAC = 0.2
ROI_X2_FRAC = 0.8
ROI_Y1_FRAC = 0.2
ROI_Y2_FRAC = 0.8

# ====== TCP Server 設定 ======
SERVER_HOST = "0.0.0.0"   # 綁定所有介面
SERVER_PORT = 9000        # 可調整 port，跟 client 要一致


def detect_has_towel(frame, model):

    # 影像大小 + ROI 座標
    h_img, w_img, _ = frame.shape
    roi_x1 = int(ROI_X1_FRAC * w_img)
    roi_x2 = int(ROI_X2_FRAC * w_img)
    roi_y1 = int(ROI_Y1_FRAC * h_img)
    roi_y2 = int(ROI_Y2_FRAC * h_img)

    results = model(frame, conf=CONF_TH)
    boxes = results[0].boxes

    has_towel = False

    for box in boxes:
        cls_id = int(box.cls[0].item())

        # 物件辨識 (認毛巾跟包包)
        if cls_id not in (TOWEL_CLASS_ID, BAG_CLASS_ID):
            continue

        x1, y1, x2, y2 = box.xyxy[0].tolist()
        cx = (x1 + x2) / 2
        cy = (y1 + y2) / 2

        # 1) 畫面中有 bag -> 直接算有
        if cls_id == BAG_CLASS_ID:
            has_towel = True
            break

        # 2) towel 在 ROI 範圍內才算有
        if cls_id == TOWEL_CLASS_ID:
            if roi_x1 <= cx <= roi_x2 and roi_y1 <= cy <= roi_y2:
                has_towel = True
                break

    return has_towel


def main():
    # 先載 YOLO
    print("[server] loading YOLO model...")
    model = YOLO(MODEL_PATH)

    # 打開攝影機
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("[server] 無法開啟攝影機")
        return

    # 建 TCP Server
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((SERVER_HOST, SERVER_PORT))
    srv.listen(1)
    print(f"[server] listening on {SERVER_HOST}:{SERVER_PORT} ...")


    try:
        # ===== 外層：不停等待新的 client =====
        while True:
            print("[server] waiting for new client ...")
            conn, addr = srv.accept()
            print(f"[server] client connected from {addr}")

            try:
                # # ===== 內層：和這個 client 溝通，直到 client 斷線 =====
                while True:
                    ret, frame = cap.read()
                    if not ret:
                        print("[server] 讀取影像失敗，結束 server")
                        return

                    has_towel = detect_has_towel(frame, model)

                    # 把訊息丟給 client （HAS / NO + 換行）
                    msg = b"HAS\n" if has_towel else b"NO\n"
                    try:
                        conn.sendall(msg)
                    except OSError:
                        print("[server] client disconnected")
                        break  # 跳出內層 while，回到外層等待下一個 client

                    # server 端自己可以查看畫面
                    annotated = frame.copy()
                    text = "HasTowel" if has_towel else "NoTowel"
                    color = (0, 255, 0) if has_towel else (0, 0, 255)
                    cv2.putText(
                        annotated,
                        text,
                        (30, 40),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        1,
                        color,
                        2,
                        cv2.LINE_AA
                    )
                    cv2.imshow("Towel Server (press q to quit)", annotated)

                    # 按 q 真的關掉整個 server（不再等新的 client）
                    if cv2.waitKey(1) & 0xFF == ord('q'):
                        print("[server] q pressed, shutting down.")
                        return

            finally:
                conn.close()
                print("[server] client connection closed")

    except KeyboardInterrupt:
        print("\n[server] KeyboardInterrupt, shutting down.")
    finally:
        srv.close()
        cap.release()
        cv2.destroyAllWindows()
        print("[server] closed")


if __name__ == "__main__":
    main()

from flask import Flask, request
import cv2
import numpy as np
import threading
from gtts import gTTS
import pygame
import io
import time
import queue

app = Flask(__name__)
img_queue = queue.Queue(maxsize=1)
app.last_warning = "CLEAR"

pygame.mixer.init()

# Tải mô hình YOLO11
try:
    from ultralytics import YOLO
    model = YOLO('yolo11s.pt') 
    # Danh sách ID COCO: 0:person, 1:bicycle, 2:car, 3:motorcycle, 5:bus, 7:truck, 56:chair, 60:dining table
    TARGET_IDS = [0, 1, 2, 3, 5, 7, 56, 60]
except Exception as e:
    print(f"Lỗi khởi động: {e}")
    exit()

def speak_vietnamese(text):
    try:
        tts = gTTS(text=text, lang='vi')
        fp = io.BytesIO()
        tts.write_to_fp(fp)
        fp.seek(0)
        pygame.mixer.music.load(fp)
        pygame.mixer.music.play()
        while pygame.mixer.music.get_busy():
            time.sleep(0.1)
    except: pass

@app.route('/upload', methods=['POST'])
def upload():
    img_data = request.data
    nparr = np.frombuffer(img_data, np.uint8)
    img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    if img is not None:
        if img_queue.full():
            try: img_queue.get_nowait()
            except: pass
        img_queue.put(img)
        return f"[[{app.last_warning}]]"
    return "FAIL"

def ai_processing_thread():
    last_speak_time = 0
    
    # Từ điển ánh sáng tên tiếng Việt
    class_map = {
        'PERSON': 'người',
        'BICYCLE': 'xe đạp', 'CAR': 'ô tô', 'MOTORCYCLE': 'xe máy', 'BUS': 'xe buýt', 'TRUCK': 'xe tải',
        'CHAIR': 'ghế', 'DINING TABLE': 'bàn'
    }

    while True:
        if not img_queue.empty():
            frame = img_queue.get()
            h, w, _ = frame.shape
            
            # CHỈ NHẬN DIỆN CÁC LỚP TRONG TARGET_IDS
            results = model(frame, conf=0.5, verbose=False, classes=TARGET_IDS)
            
            warning_text = "CLEAR"
            max_area = 0 

            for box in results[0].boxes:
                cls_id = int(box.cls[0])
                label_raw = model.names[cls_id].upper()
                label_vn = class_map.get(label_raw, 'vật cản')

                # Tọa độ và diện tích
                coords = box.xyxy[0].tolist()
                center_x = (coords[0] + coords[2]) / 2
                area_ratio = ((coords[2] - coords[0]) * (coords[3] - coords[1])) / (w * h)

                # Xác định Vị trí
                if center_x < w / 3: pos = "bên trái"
                elif center_x > 2 * w / 3: pos = "bên phải"
                else: pos = "phía trước"

                # Xác định Khoảng cách
                if area_ratio > 0.35: dist = "rất gần"
                elif area_ratio > 0.12: dist = "đang đến gần"
                else: dist = "ở xa"

                # Ưu tiên vật thể có diện tích lớn nhất (gần nhất)
                if area_ratio > max_area:
                    max_area = area_ratio
                    warning_text = f"Có {label_vn} {pos}, {dist}"

            # Phát âm thanh cảnh báo
            if warning_text != "CLEAR" and warning_text != app.last_warning:
                now = time.time()
                if now - last_speak_time > 3.0: 
                    threading.Thread(target=speak_vietnamese, args=(warning_text,)).start()
                    last_speak_time = now
            
            app.last_warning = warning_text

            # Hiển thị thực tế để theo dõi
            res_frame = results[0].plot()
            cv2.putText(res_frame, f"AI: {warning_text}", (10, 30), 2, 0.6, (0, 0, 255), 2)
            cv2.imshow("Gậy Thông Minh - AI Core", res_frame)

        if cv2.waitKey(1) & 0xFF == ord('q'): break

if __name__ == '__main__':
    # Chạy server Flask
    threading.Thread(target=lambda: app.run(host='0.0.0.0', port=5000, threaded=True, use_reloader=False)).start()
    ai_processing_thread()
from flask import Flask, request
import cv2
import numpy as np
import datetime
import os
import queue
import threading
from gtts import gTTS
import pygame
import io
import time

# --- CẤU HÌNH HỆ THỐNG ---
app = Flask(__name__)
img_queue = queue.Queue(maxsize=1)
app.last_ai_result = "CLEAR"

# Khởi tạo âm thanh Pygame để phát thông báo
pygame.mixer.init()

# Tạo thư mục lưu ảnh nếu chưa có
if not os.path.exists('captured_images'):
    os.makedirs('captured_images')

# --- TẢI MÔ HÌNH AI YOLO11 ---
print("\n--- DANG TAI AI (YOLO11s)... VUI LONG DOI ---")
try:
    from ultralytics import YOLO
    # Sử dụng YOLOv11s để đạt độ chính xác cao
    # Lưu ý: Nếu bạn tự train file riêng, hãy đổi tên thành 'best.pt'
    model = YOLO('yolo11s.pt') 
    print("--- [ OK ] AI DA SAN SANG ---")
except Exception as e:
    print(f"Lỗi khởi động AI: {e}")
    exit()

def speak_vietnamese(text):
    """Chuyển đổi văn bản thành giọng nói Tiếng Việt và phát ra loa máy tính"""
    try:
        # Từ điển dịch các nhóm vật thể sang câu cảnh báo
        dict_vn = {
            "PERSON": "Có người phía trước",
            "VEHICLE": "Xe cộ đang đến gần",
            "STAIRS": "Cẩn thận cầu thang",
            "POTHOLE": "Cảnh báo có hố ga",
            "OBSTACLE": "Có vật cản phía trước"
        }
        
        msg = dict_vn.get(text, "Có vật cản")
        
        # Chuyển text thành audio trong bộ nhớ (RAM) để tăng tốc
        tts = gTTS(text=msg, lang='vi')
        fp = io.BytesIO()
        tts.write_to_fp(fp)
        fp.seek(0)
        
        pygame.mixer.music.load(fp)
        pygame.mixer.music.play()
        while pygame.mixer.music.get_busy():
            time.sleep(0.1)
    except Exception as e:
        print(f"Lỗi phát âm thanh: {e}")

@app.route('/upload', methods=['POST'])
def upload():
    """Hàm nhận ảnh từ ESP32 gửi lên"""
    try:
        img_data = request.data
        gps_info = request.headers.get('GPS-Coord', '0,0')
        
        nparr = np.frombuffer(img_data, np.uint8)
        img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
        
        if img is not None:
            # Đẩy ảnh vào hàng đợi để xử lý ở luồng AI riêng
            if img_queue.full():
                try: img_queue.get_nowait()
                except: pass
            img_queue.put((img, gps_info))
            
            # Trả về kết quả AI hiện tại để ESP32 có thể xử lý loa trên gậy
            final_result = app.last_ai_result  
    
            # Gói lại trong một "khung" đặc biệt để ESP32 dễ tìm
            return f"[[{final_result}]]"
        return "FAIL"
    except Exception as e:
        return "ERROR"

def ai_processing_thread():
    """Luồng chính: Chạy AI nhận diện và Hiển thị"""
    last_speak_time = 0
    history = [] # Lưu lịch sử để lọc nhiễu

    print("--- Live Feed đang chạy (Nhấn 'q' tại cửa sổ ảnh để thoát) ---")
    
    while True:
        if not img_queue.empty():
            frame, gps = img_queue.get()
            
            # 1. Chạy AI YOLO11 (conf=0.5 để lọc bỏ các nhận diện yếu)
            results = model(frame, conf=0.5, verbose=False)
            
            raw_name = "CLEAR"
            if len(results[0].boxes) > 0:
                cls_id = int(results[0].boxes.cls[0])
                raw_name = model.names[cls_id].upper()

            # 2. PHÂN NHÓM VẬT THỂ THEO YÊU CẦU
            current_obj = "CLEAR"
            if raw_name == "PERSON":
                current_obj = "PERSON"
            elif raw_name in ["CAR", "MOTORCYCLE", "BUS", "TRUCK", "BICYCLE"]:
                current_obj = "VEHICLE"
            elif raw_name in ["STAIRS", "STAIRCASE"]: # Xuất hiện khi dùng model custom
                current_obj = "STAIRS"
            elif raw_name in ["POTHOLE", "HO_GA"]:    # Xuất hiện khi dùng model custom
                current_obj = "POTHOLE"
            elif raw_name != "CLEAR":
                current_obj = "OBSTACLE"

            # 3. BỘ LỌC NHIỄU (SMA - Simple Moving Average logic)
            # Chỉ xác nhận vật thể nếu nó xuất hiện trong 2 khung hình liên tiếp
            history.append(current_obj)
            if len(history) > 2: history.pop(0)
            
            final_decision = "CLEAR"
            if len(set(history)) == 1: 
                final_decision = history[0]

            # 4. XỬ LÝ PHÁT CẢNH BÁO GIỌNG NÓI
            # Chỉ phát khi có vật thể mới và cách nhau ít nhất 3 giây
            if final_decision != "CLEAR" and final_decision != app.last_ai_result:
                if (time.time() - last_speak_time > 3):
                    threading.Thread(target=speak_vietnamese, args=(final_decision,)).start()
                    last_speak_time = time.time()

            app.last_ai_result = final_decision

            # 5. HIỂN THỊ VÀ GHI LOG
            annotated_frame = results[0].plot() # Vẽ khung nhận diện của YOLO
            
            # Vẽ thông tin trạng thái lên màn hình
            status_color = (0, 0, 255) if final_decision != "CLEAR" else (0, 255, 0)
            cv2.putText(annotated_frame, f"AI STATE: {final_decision}", (10, 60), 
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, status_color, 2)
            cv2.putText(annotated_frame, f"GPS: {gps}", (10, 30), 
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
            
            cv2.imshow("XIAO ESP32-S3 AI VISION", annotated_frame)
            
            # Lưu Log vào file CSV để làm báo cáo
            if final_decision != "CLEAR":
                with open("detection_log.csv", "a", encoding="utf-8") as f:
                    now = datetime.datetime.now().strftime("%H:%M:%S")
                    f.write(f"{now},{gps},{final_decision}\n")

        # Thoát khi nhấn phím 'q'
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cv2.destroyAllWindows()
if __name__ == '__main__':
    try:
        # Chạy Server Flask ở luồng nền
        flask_thread = threading.Thread(target=lambda: app.run(host='0.0.0.0', 
                                                              port=5000, 
                                                              debug=False, 
                                                              threaded=True, 
                                                              use_reloader=False))
        flask_thread.daemon = True
        flask_thread.start()

        # Chạy luồng xử lý AI
        ai_processing_thread()
        
    except KeyboardInterrupt:
        print("\n--- DANG DUNG HE THONG... ---")
    finally:
        cv2.destroyAllWindows()
        pygame.mixer.quit()
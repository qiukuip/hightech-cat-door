from ultralytics import YOLO


model = YOLO('runs/detect/train-2/weights/best_int8.onnx') 

results = model.predict('model-val-1.jpeg')

results[0].show()
print(results[0].boxes.xyxy)

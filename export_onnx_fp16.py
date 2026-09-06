from ultralytics import YOLO


model = YOLO('runs/detect/train/weights/best.pt')
# 导出为 FP16 的 ONNX 格式
model.export(format='onnx', half=True)

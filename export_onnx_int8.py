from ultralytics import YOLO


model = YOLO('runs/detect/train/weights/best.pt')
# 导出为 INT8 格式的 ONNX
model.export(format="onnx", int8=True, data='data.yaml')

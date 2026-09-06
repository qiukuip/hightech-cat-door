from ultralytics import YOLO


model = YOLO('runs/detect/train-2/weights/best.pt')
# results = model('model-val-1.jpeg', save=True)
results = model('model-val-1.jpeg', save=False)

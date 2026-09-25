# Cat Link

YOLOv8-based custom object detection project for detecting a single class (`my-cat`).

## Overview

Trained on the [Roboflow cat-link dataset (v1)](https://universe.roboflow.com/airless-less7x-icloud-com/cat-link/dataset/1) (CC BY 4.0), fine-tuned from `yolov8n.pt` to detect a specific cat in both daytime and nighttime images.

## Dataset

- Source: Roboflow `cat-link` v1
- Classes: 1 (`my-cat`)
- Split: train / val / test (see `data.yaml`)
- Plan: 120 daytime + 120 nighttime for training, 30 + 30 for validation

## Project Structure

```
.
├── data.yaml                  # Dataset config (paths, classes, Roboflow metadata)
├── train.py                   # Train YOLOv8n on the dataset
├── validate.py                # Inference with the trained PyTorch weights
├── export_onnx_fp16.py        # Export trained weights to FP16 ONNX
├── export_onnx_int8.py        # Export trained weights to INT8 ONNX (quantized)
└── validate_onnx_int8.py      # Inference with INT8 ONNX model
```

## Requirements

- Python 3.8+
- [Ultralytics](https://github.com/ultralytics/ultralytics): `pip install ultralytics`

You also need to download `yolov8n.pt` from the Ultralytics release page (not included in this repo due to size).

## Usage

### 1. Train

```bash
python train.py
```

Trains `yolov8n.pt` for 100 epochs, image size 640, batch size 16. Output weights go to `runs/detect/train-2/weights/`.

### 2. Validate (PyTorch)

```bash
python validate.py
```

Runs inference on `model-val-1.jpeg` using `runs/detect/train-2/weights/best.pt`.

### 3. Export to ONNX

```bash
python export_onnx_fp16.py    # FP16
python export_onnx_int8.py    # INT8 (uses data.yaml for calibration)
```

### 4. Validate (INT8 ONNX)

```bash
python validate_onnx_int8.py
```

Runs inference with `best_int8.onnx` and prints bounding boxes.

## Notes

- `dataset/`, `photos/`, `runs/`, `yolov8n.pt`, and `*.onnx` are excluded via `.gitignore` — download the dataset from Roboflow and model weights from Ultralytics before running training.
- Training script is currently set to `device='cpu'`; change to `'mps'` or `'0'` for GPU acceleration if available.

## License

Dataset is CC BY 4.0 (Roboflow). Code in this repo is provided as-is.

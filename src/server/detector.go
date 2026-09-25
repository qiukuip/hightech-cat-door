package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"sync"
)

type Detector interface {
	Detect(jpeg []byte) (bool, error)
	Close() error
}

type StubDetector struct{}

func (StubDetector) Detect(jpeg []byte) (bool, error) { return false, nil }
func (StubDetector) Close() error                     { return nil }

type AlwaysYesDetector struct{}

func (AlwaysYesDetector) Detect(jpeg []byte) (bool, error) { return true, nil }
func (AlwaysYesDetector) Close() error                     { return nil }

type PythonDetector struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	reader *bufio.Reader
	mu     sync.Mutex
}

func NewPythonDetector(modelPath, scriptPath string) (*PythonDetector, error) {
	cmd := exec.Command("python3", "-u", scriptPath, "--model", modelPath)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("python detector: stdin pipe: %w", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("python detector: stdout pipe: %w", err)
	}
	cmd.Stderr = newPrefixWriter("detector")
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("python detector: start: %w", err)
	}
	return &PythonDetector{
		cmd:    cmd,
		stdin:  stdin,
		reader: bufio.NewReader(stdout),
	}, nil
}

func (d *PythonDetector) Detect(jpeg []byte) (bool, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	if len(jpeg) == 0 {
		return false, errors.New("python detector: empty jpeg")
	}
	if len(jpeg) > MaxBodySize {
		return false, fmt.Errorf("python detector: jpeg too large: %d", len(jpeg))
	}

	lenBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBuf, uint32(len(jpeg)))
	if _, err := d.stdin.Write(lenBuf); err != nil {
		return false, fmt.Errorf("python detector: write len: %w", err)
	}
	if _, err := d.stdin.Write(jpeg); err != nil {
		return false, fmt.Errorf("python detector: write jpeg: %w", err)
	}

	resp, err := d.reader.ReadByte()
	if err != nil {
		return false, fmt.Errorf("python detector: read result: %w", err)
	}
	switch resp {
	case DetectResultNotCat:
		return false, nil
	case DetectResultIsCat:
		return true, nil
	default:
		return false, fmt.Errorf("python detector: unexpected byte 0x%02X", resp)
	}
}

func (d *PythonDetector) Close() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if err := d.stdin.Close(); err != nil {
		return err
	}
	return d.cmd.Wait()
}

type prefixWriter struct {
	prefix string
}

func newPrefixWriter(prefix string) *prefixWriter { return &prefixWriter{prefix: prefix} }

func (w *prefixWriter) Write(p []byte) (int, error) {
	fmt.Printf("[%s] %s", w.prefix, p)
	return len(p), nil
}

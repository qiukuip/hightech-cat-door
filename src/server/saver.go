package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type imageSaver struct {
	baseDir string
	mu      sync.Mutex
	counter uint64
}

func newImageSaver(baseDir string) *imageSaver {
	return &imageSaver{baseDir: baseDir}
}

func (s *imageSaver) save(direction string, jpeg []byte) (string, error) {
	if s.baseDir == "" {
		return "", nil
	}
	if len(jpeg) == 0 {
		return "", fmt.Errorf("saver: empty jpeg for direction=%s", direction)
	}
	dir := filepath.Join(s.baseDir, direction)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", fmt.Errorf("saver: mkdir %s: %w", dir, err)
	}
	s.mu.Lock()
	s.counter++
	c := s.counter
	s.mu.Unlock()
	name := fmt.Sprintf("%s-%06d.jpg", time.Now().Format("20060102-150405.000"), c)
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, jpeg, 0o644); err != nil {
		return "", fmt.Errorf("saver: write %s: %w", path, err)
	}
	return path, nil
}
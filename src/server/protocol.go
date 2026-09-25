package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	Magic byte = 0xAA
)

const (
	TypeHeartbeat         byte = 0x0
	TypeUploadAndDetect   byte = 0x1
	TypeDetectResult      byte = 0x2
	TypeRequestCapture    byte = 0x3
	TypeCaptureAndStore   byte = 0x4
	TypeManualOpen        byte = 0x5
	TypeManualClose       byte = 0x6
	TypeDoorStateReport   byte = 0x7
	TypeRegister          byte = 0x8
	TypeIndoorTriggerOpen byte = 0x9
	TypeOutdoorPassage    byte = 0xA
	TypeDisconnect        byte = 0xF
)

const (
	RoleOutdoor byte = 0x0
	RoleIndoor  byte = 0x1
)

const (
	HeaderSize   = 4
	MaxFrameSize = 65535
	MaxBodySize  = MaxFrameSize - HeaderSize
)

const (
	DetectResultNotCat byte = 0x0
	DetectResultIsCat  byte = 0x1
)

const (
	DoorStateClosed  byte = 0x0
	DoorStateOpening byte = 0x1
	DoorStateOpen    byte = 0x2
	DoorStateClosing byte = 0x3
	DoorStatePassage byte = 0x4
)

var (
	ErrBadMagic     = errors.New("protocol: bad magic byte")
	ErrShortFrame   = errors.New("protocol: total length smaller than header")
	ErrFrameTooLong = errors.New("protocol: frame exceeds max size")
)

type Frame struct {
	Type byte
	Body []byte
}

func (f Frame) Encode() ([]byte, error) {
	if int(f.Type) < 0 {
		return nil, fmt.Errorf("protocol: invalid type 0x%X", f.Type)
	}
	if len(f.Body) > MaxBodySize {
		return nil, ErrFrameTooLong
	}
	total := uint16(HeaderSize + len(f.Body))
	buf := make([]byte, total)
	buf[0] = Magic
	buf[1] = f.Type
	binary.LittleEndian.PutUint16(buf[2:4], total)
	copy(buf[HeaderSize:], f.Body)
	return buf, nil
}

func Decode(r io.Reader) (Frame, error) {
	var hdr [HeaderSize]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return Frame{}, err
	}
	if hdr[0] != Magic {
		return Frame{}, fmt.Errorf("%w: got 0x%02X", ErrBadMagic, hdr[0])
	}
	total := binary.LittleEndian.Uint16(hdr[2:4])
	if total < HeaderSize {
		return Frame{}, fmt.Errorf("%w: total=%d", ErrShortFrame, total)
	}
	if total > MaxFrameSize {
		return Frame{}, fmt.Errorf("%w: total=%d", ErrFrameTooLong, total)
	}
	bodyLen := int(total) - HeaderSize
	body := make([]byte, bodyLen)
	if bodyLen > 0 {
		if _, err := io.ReadFull(r, body); err != nil {
			return Frame{}, err
		}
	}
	return Frame{Type: hdr[1], Body: body}, nil
}

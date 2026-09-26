package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

const (
	defaultListenAddr = ":1234"
	defaultModelPath  = "/home/longkun/Documents/projects/hightech-cat-door/model-training/best_int8.onnx"
	defaultScriptPath = "/home/longkun/Documents/projects/hightech-cat-door/model-training/detect.py"
	defaultSaveDir    = "/home/longkun/Pictures/hightech-cat-door"
	defaultDBDSN      = "postgresql://postgres:postgres@localhost:5432/postgres"
	openHoldDuration  = 30 * time.Second
	readIdleTimeout   = 60 * time.Second
	writeDeadline     = 10 * time.Second
	registerTimeout   = 5 * time.Second
	heartbeatInterval = 25 * time.Second
)

type peerRole int

const (
	roleOutdoor peerRole = iota
	roleIndoor
)

var peerRoleName = map[peerRole]string{
	roleOutdoor: "outdoor",
	roleIndoor:  "indoor",
}

type Server struct {
	listenAddr  string
	detector    Detector
	logger      *log.Logger
	saver       *imageSaver
	eventLogger EventLogger

	mu    sync.Mutex
	peers map[peerRole]*peer

	door *doorController
}

type peer struct {
	role peerRole
	conn net.Conn
	mu   sync.Mutex
}

func (p *peer) writeFrame(f Frame) error {
	buf, err := f.Encode()
	if err != nil {
		return err
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	_ = p.conn.SetWriteDeadline(time.Now().Add(writeDeadline))
	_, err = p.conn.Write(buf)
	return err
}

func main() {
	var (
		listenAddr   = flag.String("listen", defaultListenAddr, "TCP listen address (both esp32out and esp32in connect here; role is announced via 0x8 register frame)")
		detectorKind = flag.String("detector", "python", "stub | always | python")
		modelPath    = flag.String("model", defaultModelPath, "ONNX model path (python detector)")
		scriptPath   = flag.String("script", defaultScriptPath, "python helper script")
		saveDir      = flag.String("save-dir", defaultSaveDir, "if non-empty, save every JPEG that triggered a cat event (entry: detector says cat; exit: indoor radar trigger) into <save-dir>/{outdoor,indoor}/. empty disables saving.")
		dbDSN        = flag.String("db-dsn", defaultDBDSN, "PostgreSQL DSN for door-event and detection logging; empty disables DB logging (waiting for configuration).")
	)
	flag.Parse()

	logger := log.New(os.Stdout, "", log.LstdFlags|log.Lmicroseconds)

	detector, closer, err := buildDetector(*detectorKind, *modelPath, *scriptPath, logger)
	if err != nil {
		logger.Fatalf("init detector: %v", err)
	}
	if closer != nil {
		defer closer.Close()
	}

	var eventLogger EventLogger = NoopLogger{}
	if *dbDSN != "" {
		pl, err := NewPostgresLogger(context.Background(), *dbDSN, logger)
		if err != nil {
			logger.Fatalf("init postgres logger: %v", err)
		}
		eventLogger = pl
		defer eventLogger.Close()
		logger.Printf("db: connected, door events and detections will be logged")
	} else {
		logger.Printf("db: disabled (--db-dsn empty)")
	}

	s := &Server{
		listenAddr:  *listenAddr,
		detector:    detector,
		logger:      logger,
		saver:       newImageSaver(*saveDir),
		eventLogger: eventLogger,
		peers:       map[peerRole]*peer{},
		door:        &doorController{},
	}
	s.door.server = s

	if *saveDir != "" {
		logger.Printf("image save dir: %s", *saveDir)
	} else {
		logger.Printf("image save dir: disabled")
	}

	if err := s.run(); err != nil {
		logger.Fatalf("server: %v", err)
	}
}

func buildDetector(kind, modelPath, scriptPath string, logger *log.Logger) (Detector, Detector, error) {
	switch kind {
	case "stub":
		logger.Printf("detector: stub (always returns not-cat)")
		return StubDetector{}, nil, nil
	case "always":
		logger.Printf("detector: always-yes (always returns cat)")
		return AlwaysYesDetector{}, nil, nil
	case "python":
		logger.Printf("detector: python (model=%s script=%s)", modelPath, scriptPath)
		d, err := NewPythonDetector(modelPath, scriptPath)
		if err != nil {
			return nil, nil, err
		}
		return d, d, nil
	default:
		return nil, nil, fmt.Errorf("unknown detector %q (want stub|always|python)", kind)
	}
}

func (s *Server) run() error {
	l, err := net.Listen("tcp", s.listenAddr)
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	defer l.Close()

	s.logger.Printf("listening %s (single port, role-via-0x8-register) detector=%T", l.Addr(), s.detector)

	var wg sync.WaitGroup
	wg.Go(func() {
		s.acceptLoop(l)
	})
	wg.Go(func() {
		s.heartbeatLoop()
	})

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	sig := <-sigCh
	s.logger.Printf("signal received: %v; shutting down", sig)

	_ = l.Close()
	wg.Wait()
	return nil
}

func (s *Server) acceptLoop(l net.Listener) {
	for {
		conn, err := l.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			s.logger.Printf("accept: %v", err)
			return
		}
		s.logger.Printf("accepted: %s", conn.RemoteAddr())
		go s.handle(conn)
	}
}

func (s *Server) heartbeatLoop() {
	t := time.NewTicker(heartbeatInterval)
	defer t.Stop()
	for range t.C {
		for _, role := range s.peersSnapshot() {
			p := s.getPeer(role)
			if p == nil {
				continue
			}
			if err := p.writeFrame(Frame{Type: TypeHeartbeat}); err != nil {
				s.logger.Printf("%s heartbeat write: %v", peerRoleName[role], err)
			}
		}
	}
}

func (s *Server) peersSnapshot() []peerRole {
	s.mu.Lock()
	defer s.mu.Unlock()
	roles := make([]peerRole, 0, len(s.peers))
	for r := range s.peers {
		roles = append(roles, r)
	}
	return roles
}

func (s *Server) handle(conn net.Conn) {
	defer conn.Close()

	role, ok := s.readRegister(conn)
	if !ok {
		return
	}
	s.registerPeer(role, conn)
	defer s.unregisterPeer(role, conn)

	switch role {
	case roleOutdoor:
		s.readLoop(peerRoleName[role], conn, s.onOutdoorFrame)
	case roleIndoor:
		s.readLoop(peerRoleName[role], conn, s.onIndoorFrame)
	}
}

func (s *Server) readRegister(conn net.Conn) (peerRole, bool) {
	_ = conn.SetReadDeadline(time.Now().Add(registerTimeout))
	f, err := Decode(conn)
	if err != nil {
		s.logger.Printf("register read failed from %s: %v", conn.RemoteAddr(), err)
		return 0, false
	}
	if f.Type != TypeRegister || len(f.Body) != 1 {
		s.logger.Printf("expected register frame from %s, got type=0x%X len=%d", conn.RemoteAddr(), f.Type, len(f.Body))
		return 0, false
	}
	switch f.Body[0] {
	case RoleOutdoor:
		return roleOutdoor, true
	case RoleIndoor:
		return roleIndoor, true
	default:
		s.logger.Printf("unknown role 0x%02X from %s", f.Body[0], conn.RemoteAddr())
		return 0, false
	}
}

func (s *Server) registerPeer(role peerRole, conn net.Conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if existing, ok := s.peers[role]; ok {
		s.logger.Printf("%s: closing stale peer %s", peerRoleName[role], existing.conn.RemoteAddr())
		existing.conn.Close()
	}
	s.peers[role] = &peer{role: role, conn: conn}
	s.logger.Printf("%s registered: %s", peerRoleName[role], conn.RemoteAddr())
}

func (s *Server) unregisterPeer(role peerRole, conn net.Conn) {
	s.mu.Lock()
	if p, ok := s.peers[role]; ok && p.conn == conn {
		delete(s.peers, role)
	}
	s.mu.Unlock()
	s.logger.Printf("%s disconnected: %s", peerRoleName[role], conn.RemoteAddr())
}

func (s *Server) getPeer(role peerRole) *peer {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.peers[role]
}

func (s *Server) readLoop(name string, conn net.Conn, handler func(Frame) error) {
	for {
		_ = conn.SetReadDeadline(time.Now().Add(readIdleTimeout))
		f, err := Decode(conn)
		if err != nil {
			if errors.Is(err, io.EOF) || isNetTimeout(err) {
				s.logger.Printf("%s read: %v", name, err)
			} else {
				s.logger.Printf("%s read error: %v", name, err)
			}
			return
		}
		if err := handler(f); err != nil {
			s.logger.Printf("%s handler error: %v", name, err)
			return
		}
	}
}

func isNetTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

func (s *Server) onOutdoorFrame(f Frame) error {
	switch f.Type {
	case TypeHeartbeat:
		return nil
	case TypeUploadAndDetect:
		s.logger.Printf("detection: %d bytes jpeg", len(f.Body))
		isCat, err := s.detector.Detect(f.Body)
		if err != nil {
			s.logger.Printf("detector error: %v (responding not-cat)", err)
			isCat = false
		}
		result := byte(DetectResultNotCat)
		if isCat {
			result = DetectResultIsCat
		}
		oc := s.getPeer(roleOutdoor)
		if oc == nil {
			s.logger.Printf("detection done but outdoor disconnected")
			return nil
		}
		if err := oc.writeFrame(Frame{Type: TypeDetectResult, Body: []byte{result}}); err != nil {
			s.logger.Printf("send detect result: %v", err)
		}
		if isCat {
			path, err := s.saver.save("outdoor", f.Body)
			if err != nil {
				s.logger.Printf("save outdoor jpeg: %v", err)
			}
			if path != "" {
				s.eventLogger.LogDetection(DetectionEvent{
					Direction: "outdoor",
					ImagePath: path,
					Result:    "cat",
				})
			}
			s.door.requestOpen(roleOutdoor)
		}
		return nil
	case TypeOutdoorPassage:
		s.door.onOutdoorPassage()
		return nil
	case TypeDoorStateReport:
		if len(f.Body) >= 1 {
			s.logger.Printf("outdoor door-state-report: 0x%02X", f.Body[0])
		}
		return nil
	case TypeRegister:
		s.logger.Printf("outdoor re-sent register (ignored)")
		return nil
	case TypeDisconnect:
		s.logger.Printf("outdoor sent disconnect")
		return io.EOF
	default:
		s.logger.Printf("outdoor unknown frame type 0x%X", f.Type)
		return nil
	}
}

func (s *Server) onIndoorFrame(f Frame) error {
	switch f.Type {
	case TypeHeartbeat:
		return nil
	case TypeIndoorTriggerOpen:
		if s.door.phase() == doorIdle {
			s.logger.Printf("indoor trigger: %d bytes jpeg (exit flow, skipping detect)", len(f.Body))
		} else {
			s.logger.Printf("indoor trigger: %d bytes jpeg (door %s; saved only, cat likely entering)", len(f.Body), doorPhaseName[s.door.phase()])
		}
		path, err := s.saver.save("indoor", f.Body)
		if err != nil {
			s.logger.Printf("save indoor jpeg: %v", err)
		}
		if path != "" {
			s.eventLogger.LogDetection(DetectionEvent{
				Direction: "indoor",
				ImagePath: path,
				Result:    "cat",
			})
		}
		s.door.requestOpen(roleIndoor)
		return nil
	case TypeDoorStateReport:
		if len(f.Body) < 1 {
			s.logger.Printf("indoor door-state-report: empty body")
			return nil
		}
		s.door.onIndoorState(f.Body[0])
		return nil
	case TypeRegister:
		s.logger.Printf("indoor re-sent register (ignored)")
		return nil
	case TypeDisconnect:
		s.logger.Printf("indoor sent disconnect")
		return io.EOF
	default:
		s.logger.Printf("indoor unknown frame type 0x%X", f.Type)
		return nil
	}
}

const (
	doorIdle doorPhase = iota
	doorOpening
	doorOpen
	doorClosing
)

var doorPhaseName = map[doorPhase]string{
	doorIdle:    "idle",
	doorOpening: "opening",
	doorOpen:    "open",
	doorClosing: "closing",
}

type doorPhase int

type doorController struct {
	server          *Server
	mu              sync.Mutex
	state           doorPhase
	timer           *time.Timer
	expectedPassage peerRole
}

func oppositeRole(r peerRole) peerRole {
	if r == roleOutdoor {
		return roleIndoor
	}
	return roleOutdoor
}

func (d *doorController) phase() doorPhase {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.state
}

func (d *doorController) requestOpen(triggerSource peerRole) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.state != doorIdle {
		d.server.logger.Printf("door: open ignored, state=%s (trigger=%s)", doorPhaseName[d.state], peerRoleName[triggerSource])
		return
	}
	ic := d.server.getPeer(roleIndoor)
	if ic == nil {
		d.server.logger.Printf("door: open failed, indoor offline")
		return
	}
	d.state = doorOpening
	d.expectedPassage = oppositeRole(triggerSource)
	d.server.logger.Printf("door: opening (trigger=%s, expected_passage=%s)",
		peerRoleName[triggerSource], peerRoleName[d.expectedPassage])
	if err := ic.writeFrame(Frame{Type: TypeManualOpen}); err != nil {
		d.server.logger.Printf("door: send open: %v", err)
		d.state = doorIdle
		return
	}
	d.server.eventLogger.LogDoorEvent(DoorEvent{
		Event:  "open_request",
		Source: peerRoleName[triggerSource],
		Reason: "request",
	})
}

func (d *doorController) requestClose(reason string) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.state != doorOpen {
		return
	}
	if d.timer != nil {
		d.timer.Stop()
		d.timer = nil
	}
	ic := d.server.getPeer(roleIndoor)
	if ic == nil {
		d.server.logger.Printf("door: close (%s) failed, indoor offline; resetting", reason)
		d.state = doorIdle
		return
	}
	d.state = doorClosing
	d.server.logger.Printf("door: closing (%s)", reason)
	if err := ic.writeFrame(Frame{Type: TypeManualClose}); err != nil {
		d.server.logger.Printf("door: send close: %v", err)
		d.state = doorIdle
		return
	}
	d.server.eventLogger.LogDoorEvent(DoorEvent{
		Event:  "close_request",
		Source: peerRoleName[d.expectedPassage],
		Reason: reason,
	})
}

func (d *doorController) onIndoorState(state byte) {
	d.mu.Lock()
	defer d.mu.Unlock()
	switch state {
	case DoorStateOpen:
		if d.state != doorOpening {
			return
		}
		d.state = doorOpen
		d.server.logger.Printf("door: open confirmed, starting %s hold timer (expected_passage=%s)",
			openHoldDuration, peerRoleName[d.expectedPassage])
		d.server.eventLogger.LogDoorEvent(DoorEvent{
			Event:  "open_confirmed",
			Source: "indoor",
			Reason: "indoor-state",
		})
		d.timer = time.AfterFunc(openHoldDuration, func() {
			d.requestClose("timeout")
		})
	case DoorStatePassage:
		if d.state != doorOpen {
			return
		}
		if d.expectedPassage != roleIndoor {
			d.server.logger.Printf("door: indoor passage ignored (expected_passage=%s)",
				peerRoleName[d.expectedPassage])
			return
		}
		d.server.logger.Printf("door: passage detected by indoor radar")
		go d.requestClose("passage")
	case DoorStateClosed:
		if d.state != doorClosing {
			return
		}
		if d.timer != nil {
			d.timer.Stop()
			d.timer = nil
		}
		d.state = doorIdle
		d.expectedPassage = roleOutdoor
		d.server.logger.Printf("door: closed")
		d.server.eventLogger.LogDoorEvent(DoorEvent{
			Event:  "closed",
			Source: "indoor",
			Reason: "indoor-state",
		})
	default:
		d.server.logger.Printf("door: unknown indoor state 0x%02X", state)
	}
}

func (d *doorController) onOutdoorPassage() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.state != doorOpen {
		return
	}
	if d.expectedPassage != roleOutdoor {
		d.server.logger.Printf("door: outdoor passage ignored (expected_passage=%s)",
			peerRoleName[d.expectedPassage])
		return
	}
	d.server.logger.Printf("door: passage detected by outdoor radar")
	go d.requestClose("passage-outdoor")
}

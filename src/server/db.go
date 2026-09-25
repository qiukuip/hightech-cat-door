package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const dbWriteTimeout = 5 * time.Second

const schemaSQL = `
CREATE TABLE IF NOT EXISTS door_events (
    id          BIGSERIAL PRIMARY KEY,
    event       TEXT        NOT NULL,
    source      TEXT        NOT NULL,
    reason      TEXT        NOT NULL DEFAULT '',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS detections (
    id          BIGSERIAL PRIMARY KEY,
    direction   TEXT        NOT NULL,
    image_path  TEXT        NOT NULL,
    result      TEXT        NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS door_events_created_at_idx ON door_events (created_at DESC);
CREATE INDEX IF NOT EXISTS detections_created_at_idx  ON detections  (created_at DESC);
`

type DoorEvent struct {
	Event  string
	Source string
	Reason string
}

type DetectionEvent struct {
	Direction string
	ImagePath string
	Result    string
}

type EventLogger interface {
	LogDoorEvent(DoorEvent)
	LogDetection(DetectionEvent)
	Close() error
}

type NoopLogger struct{}

func (NoopLogger) LogDoorEvent(DoorEvent)      {}
func (NoopLogger) LogDetection(DetectionEvent) {}
func (NoopLogger) Close() error                { return nil }

type PostgresLogger struct {
	pool   *pgxpool.Pool
	queue  chan func(context.Context) error
	logger *log.Logger
}

func NewPostgresLogger(ctx context.Context, dsn string, logger *log.Logger) (*PostgresLogger, error) {
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, fmt.Errorf("db: parse dsn: %w", err)
	}
	cfg.MaxConns = 4
	cfg.MinConns = 1
	cfg.MaxConnLifetime = 30 * time.Minute
	cfg.MaxConnIdleTime = 5 * time.Minute
	cfg.HealthCheckPeriod = 30 * time.Second

	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("db: connect: %w", err)
	}

	pingCtx, cancel := context.WithTimeout(ctx, dbWriteTimeout)
	defer cancel()
	if err := pool.Ping(pingCtx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("db: ping: %w", err)
	}

	if _, err := pool.Exec(ctx, schemaSQL); err != nil {
		pool.Close()
		return nil, fmt.Errorf("db: apply schema: %w", err)
	}

	l := &PostgresLogger{
		pool:   pool,
		queue:  make(chan func(context.Context) error, 256),
		logger: logger,
	}
	go l.run()
	return l, nil
}

func (l *PostgresLogger) run() {
	for fn := range l.queue {
		ctx, cancel := context.WithTimeout(context.Background(), dbWriteTimeout)
		if err := fn(ctx); err != nil && !errors.Is(err, context.Canceled) {
			l.logger.Printf("db: insert failed: %v", err)
		}
		cancel()
	}
}

func (l *PostgresLogger) LogDoorEvent(e DoorEvent) {
	fn := func(ctx context.Context) error {
		_, err := l.pool.Exec(ctx,
			`INSERT INTO door_events (event, source, reason) VALUES ($1, $2, $3)`,
			e.Event, e.Source, e.Reason)
		return err
	}
	select {
	case l.queue <- fn:
	default:
		l.logger.Printf("db: door-event queue full, dropping event=%s source=%s", e.Event, e.Source)
	}
}

func (l *PostgresLogger) LogDetection(d DetectionEvent) {
	fn := func(ctx context.Context) error {
		_, err := l.pool.Exec(ctx,
			`INSERT INTO detections (direction, image_path, result) VALUES ($1, $2, $3)`,
			d.Direction, d.ImagePath, d.Result)
		return err
	}
	select {
	case l.queue <- fn:
	default:
		l.logger.Printf("db: detection queue full, dropping direction=%s", d.Direction)
	}
}

func (l *PostgresLogger) Close() error {
	close(l.queue)
	l.pool.Close()
	return nil
}

// Package store 是 PartyUp 服务端的持久层（SQLite）。
//
// 设计取舍（见 docs/ARCHITECTURE.md §6）：单二进制 + 单文件库，备份 = 停服拷贝
// data/。驱动用 modernc.org/sqlite（纯 Go，无 cgo），这样 WSL 里 go build 与
// 交叉编译到 Windows 都不需要 C 工具链。
//
// 并发模型：database/sql 连接池固定为 1 条连接。SQLite 的写者本来就是串行的，
// 单连接把「锁竞争」提前变成 Go 侧的排队，行为可预测；ASCII 小规模（2–12 人）
// 下性能完全够用。所有查询都带 context，便于测试与优雅停机时取消。
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/oklog/ulid/v2"
	sqlite3 "modernc.org/sqlite"
)

// 领域错误。上层据此映射协议错误码：ErrNotFound → NOT_FOUND，
// ErrConflict → CONFLICT，其余 → INTERNAL。
var (
	ErrNotFound = errors.New("store: not found")
	ErrConflict = errors.New("store: conflict")

	// 邀请码注册的两个具体冲突原因，给客户端更准确的提示。
	ErrInviteUsed = errors.New("store: invite already used")
	ErrNameTaken  = errors.New("store: name already taken")
)

// 领域对象。字段与 protocol 包里的同名结构一一对应，转换在 server 层做，
// 这样持久层不依赖协议（也不需要为了一个 JSON tag 去 import protocol）。
type Member struct {
	ID        string
	Name      string
	Admin     bool
	CreatedAt int64
}

// MemberRecord 是带密码哈希的成员记录，只在登录/注册路径出现。
type MemberRecord struct {
	Member
	PasswordHash string
}

// Invite 是邀请码。
type Invite struct {
	Code      string
	CreatedBy string
	CreatedAt int64
	UsedBy    string
	UsedAt    int64
}

// Channel 是频道。骨架只持久化 id/name/created_at，三区内容在内存里（语音/共享为空）。
type Channel struct {
	ID        string
	Name      string
	CreatedAt int64
}

// Message 是一条文字消息。MemberName 由 History 联表带出（实时广播时由 hub 填
// 当前连接的身份），这样历史消息里的离线成员也能显示名字。
type Message struct {
	ID         string
	ChannelID  string
	Seq        int64
	MemberID   string
	MemberName string
	Body       string
	TS         int64
}

// Store 是库句柄。
type Store struct {
	db   *sql.DB
	path string
}

// schema 是建表语句集合。用 IF NOT EXISTS，启动时幂等执行；骨架不做版本化迁移，
// 下一版引入 schema_version 表与迁移函数。
var schema = []string{
	`CREATE TABLE IF NOT EXISTS settings (
		key   TEXT PRIMARY KEY,
		value TEXT NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS members (
		id            TEXT PRIMARY KEY,
		name          TEXT NOT NULL COLLATE NOCASE UNIQUE,
		password_hash TEXT NOT NULL,
		is_admin      INTEGER NOT NULL DEFAULT 0,
		created_at    INTEGER NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS sessions (
		token_hash TEXT PRIMARY KEY,
		member_id  TEXT NOT NULL REFERENCES members(id) ON DELETE CASCADE,
		created_at INTEGER NOT NULL,
		revoked_at INTEGER
	)`,
	`CREATE INDEX IF NOT EXISTS sessions_member ON sessions(member_id)`,
	`CREATE TABLE IF NOT EXISTS invites (
		code       TEXT PRIMARY KEY,
		created_by TEXT REFERENCES members(id),
		created_at INTEGER NOT NULL,
		used_by    TEXT REFERENCES members(id),
		used_at    INTEGER
	)`,
	`CREATE TABLE IF NOT EXISTS channels (
		id         TEXT PRIMARY KEY,
		name       TEXT NOT NULL COLLATE NOCASE UNIQUE,
		created_at INTEGER NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS messages (
		id         TEXT PRIMARY KEY,
		channel_id TEXT NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
		seq        INTEGER NOT NULL,
		member_id  TEXT NOT NULL REFERENCES members(id),
		body       TEXT NOT NULL,
		ts         INTEGER NOT NULL,
		UNIQUE (channel_id, seq)
	)`,
	`CREATE INDEX IF NOT EXISTS messages_channel_seq ON messages(channel_id, seq)`,
}

// Open 打开（必要时创建）数据库并应用 schema。dir 不存在时递归创建。
func Open(dir string) (*Store, error) {
	if dir == "" {
		return nil, errors.New("store: empty data dir")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("store: create data dir: %w", err)
	}
	path := filepath.Join(dir, "partyup.db")

	// pragma 通过 DSN 注入：WAL 让读不阻塞写，busy_timeout 吸收短时锁竞争。
	dsn := "file:" + path + "?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("store: open %s: %w", path, err)
	}
	db.SetMaxOpenConns(1)
	db.SetConnMaxLifetime(0)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := db.PingContext(ctx); err != nil {
		db.Close()
		return nil, fmt.Errorf("store: ping %s: %w", path, err)
	}
	for _, stmt := range schema {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			db.Close()
			return nil, fmt.Errorf("store: apply schema: %w", err)
		}
	}
	return &Store{db: db, path: path}, nil
}

// Close 关闭库。
func (s *Store) Close() error { return s.db.Close() }

// Path 返回数据库文件路径（测试与运维输出用）。
func (s *Store) Path() string { return s.path }

// isConstraint 判断错误是否是唯一约束冲突（成员名/频道名/邀请码重复）。
//
// SQLITE_CONSTRAINT_UNIQUE = 2067、SQLITE_CONSTRAINT_PRIMARYKEY = 1555，
// 见 https://www.sqlite.org/rescode.html#constraint_unique。现代驱动把它们
// 包成 *sqlite.Error，用 errors.As 取原始返回码，而不是去匹配错误字符串。
func isConstraint(err error) bool {
	var se *sqlite3.Error
	if errors.As(err, &se) {
		switch se.Code() {
		case 2067, 1555: // UNIQUE / PRIMARY KEY
			return true
		}
	}
	return false
}

func newID() string { return ulid.Make().String() }

// ------------------------------------------------------------------
// settings（bootstrap token 等内部状态）
// ------------------------------------------------------------------

// GetSetting 读一个内部设置。不存在返回 ErrNotFound。
func (s *Store) GetSetting(ctx context.Context, key string) (string, error) {
	var v string
	err := s.db.QueryRowContext(ctx, `SELECT value FROM settings WHERE key = ?`, key).Scan(&v)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("store: get setting %q: %w", key, err)
	}
	return v, nil
}

// SetSetting 写入一个内部设置。
func (s *Store) SetSetting(ctx context.Context, key, value string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO settings (key, value) VALUES (?, ?)
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, value)
	if err != nil {
		return fmt.Errorf("store: set setting %q: %w", key, err)
	}
	return nil
}

// DeleteSetting 删除一个内部设置（不存在不报错）。
func (s *Store) DeleteSetting(ctx context.Context, key string) error {
	if _, err := s.db.ExecContext(ctx, `DELETE FROM settings WHERE key = ?`, key); err != nil {
		return fmt.Errorf("store: delete setting %q: %w", key, err)
	}
	return nil
}

// ------------------------------------------------------------------
// members
// ------------------------------------------------------------------

// MemberCount 返回成员总数（bootstrap 用：0 表示还没初始化）。
func (s *Store) MemberCount(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM members`).Scan(&n); err != nil {
		return 0, fmt.Errorf("store: count members: %w", err)
	}
	return n, nil
}

// CreateMember 创建成员。名字重复返回 ErrConflict。
func (s *Store) CreateMember(ctx context.Context, name, passwordHash string, admin bool) (Member, error) {
	m := Member{ID: newID(), Name: name, Admin: admin, CreatedAt: time.Now().UnixMilli()}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO members (id, name, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?, ?)`,
		m.ID, m.Name, passwordHash, boolInt(admin), m.CreatedAt)
	if isConstraint(err) {
		return Member{}, ErrConflict
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: create member: %w", err)
	}
	return m, nil
}

// MemberByName 按名字取成员及其密码哈希（登录用）。不存在返回 ErrNotFound。
func (s *Store) MemberByName(ctx context.Context, name string) (MemberRecord, error) {
	var r MemberRecord
	var admin int
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, password_hash, is_admin, created_at FROM members WHERE name = ?`, name).
		Scan(&r.ID, &r.Name, &r.PasswordHash, &admin, &r.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return MemberRecord{}, ErrNotFound
	}
	if err != nil {
		return MemberRecord{}, fmt.Errorf("store: member by name: %w", err)
	}
	r.Admin = admin != 0
	return r, nil
}

// ------------------------------------------------------------------
// sessions
// ------------------------------------------------------------------

// CreateSession 记录一个会话 token 的哈希。明文 token 只出现在响应里。
func (s *Store) CreateSession(ctx context.Context, memberID, tokenHash string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO sessions (token_hash, member_id, created_at) VALUES (?, ?, ?)`,
		tokenHash, memberID, time.Now().UnixMilli())
	if err != nil {
		return fmt.Errorf("store: create session: %w", err)
	}
	return nil
}

// MemberBySession 用 token 哈希换成员；token 不存在或已撤销返回 ErrNotFound。
func (s *Store) MemberBySession(ctx context.Context, tokenHash string) (Member, error) {
	var m Member
	var admin int
	err := s.db.QueryRowContext(ctx,
		`SELECT m.id, m.name, m.is_admin, m.created_at
		   FROM sessions s JOIN members m ON m.id = s.member_id
		  WHERE s.token_hash = ? AND s.revoked_at IS NULL`, tokenHash).
		Scan(&m.ID, &m.Name, &admin, &m.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Member{}, ErrNotFound
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: member by session: %w", err)
	}
	m.Admin = admin != 0
	return m, nil
}

// RevokeSession 撤销一个 token（"踢下线"能力的底座）。
func (s *Store) RevokeSession(ctx context.Context, tokenHash string) error {
	if _, err := s.db.ExecContext(ctx,
		`UPDATE sessions SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL`,
		time.Now().UnixMilli(), tokenHash); err != nil {
		return fmt.Errorf("store: revoke session: %w", err)
	}
	return nil
}

// ------------------------------------------------------------------
// invites
// ------------------------------------------------------------------

// CreateInvite 创建邀请码。code 重复返回 ErrConflict（调用方重试即可）。
func (s *Store) CreateInvite(ctx context.Context, code, createdBy string) (Invite, error) {
	inv := Invite{Code: code, CreatedBy: createdBy, CreatedAt: time.Now().UnixMilli()}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO invites (code, created_by, created_at) VALUES (?, ?, ?)`,
		inv.Code, nullableString(createdBy), inv.CreatedAt)
	if isConstraint(err) {
		return Invite{}, ErrConflict
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: create invite: %w", err)
	}
	return inv, nil
}

// InviteByCode 查邀请码。不存在返回 ErrNotFound。
func (s *Store) InviteByCode(ctx context.Context, code string) (Invite, error) {
	var inv Invite
	var createdBy, usedBy sql.NullString
	var usedAt sql.NullInt64
	err := s.db.QueryRowContext(ctx,
		`SELECT code, created_by, created_at, used_by, used_at FROM invites WHERE code = ?`, code).
		Scan(&inv.Code, &createdBy, &inv.CreatedAt, &usedBy, &usedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Invite{}, ErrNotFound
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: invite by code: %w", err)
	}
	inv.CreatedBy = createdBy.String
	inv.UsedBy = usedBy.String
	inv.UsedAt = usedAt.Int64
	return inv, nil
}

// ConsumeInvite 把邀请码标记为已用。已被用掉（并发）返回 ErrConflict。
func (s *Store) ConsumeInvite(ctx context.Context, code, memberID string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ? AND used_by IS NULL`,
		memberID, time.Now().UnixMilli(), code)
	if err != nil {
		return fmt.Errorf("store: consume invite: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: consume invite rows: %w", err)
	}
	if n == 0 {
		return ErrConflict
	}
	return nil
}

// RegisterWithInvite 在一次事务里「校验邀请码未用 → 建成员 → 标记邀请码已用」。
//
// 三步要么全成要么全不成：否则会出现「邀请码被烧掉但成员没建成」或反过来的半截状态。
// 返回 ErrNotFound（邀请码不存在）、ErrInviteUsed、ErrNameTaken。
func (s *Store) RegisterWithInvite(ctx context.Context, code, name, passwordHash string) (Member, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Member{}, fmt.Errorf("store: begin invite register: %w", err)
	}
	defer func() { _ = tx.Rollback() }() // Commit 成功后 Rollback 是空操作

	var usedBy sql.NullString
	err = tx.QueryRowContext(ctx, `SELECT used_by FROM invites WHERE code = ?`, code).Scan(&usedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return Member{}, ErrNotFound
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: check invite: %w", err)
	}
	if usedBy.Valid && usedBy.String != "" {
		return Member{}, ErrInviteUsed
	}

	m := Member{ID: newID(), Name: name, Admin: false, CreatedAt: time.Now().UnixMilli()}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO members (id, name, password_hash, is_admin, created_at) VALUES (?, ?, ?, 0, ?)`,
		m.ID, m.Name, passwordHash, m.CreatedAt); err != nil {
		if isConstraint(err) {
			return Member{}, ErrNameTaken
		}
		return Member{}, fmt.Errorf("store: register member: %w", err)
	}

	res, err := tx.ExecContext(ctx,
		`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ? AND used_by IS NULL`,
		m.ID, time.Now().UnixMilli(), code)
	if err != nil {
		return Member{}, fmt.Errorf("store: mark invite used: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return Member{}, fmt.Errorf("store: mark invite used rows: %w", err)
	}
	if n == 0 {
		return Member{}, ErrInviteUsed // 并发抢同一个码
	}

	if err := tx.Commit(); err != nil {
		return Member{}, fmt.Errorf("store: commit invite register: %w", err)
	}
	return m, nil
}

// FirstUnusedInvite 返回最早创建且未使用的邀请码；没有则 ErrNotFound。
func (s *Store) FirstUnusedInvite(ctx context.Context) (Invite, error) {
	var inv Invite
	var createdBy sql.NullString
	err := s.db.QueryRowContext(ctx,
		`SELECT code, created_by, created_at FROM invites
		  WHERE used_by IS NULL ORDER BY created_at ASC LIMIT 1`).
		Scan(&inv.Code, &createdBy, &inv.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Invite{}, ErrNotFound
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: first unused invite: %w", err)
	}
	inv.CreatedBy = createdBy.String
	return inv, nil
}

// ------------------------------------------------------------------
// channels
// ------------------------------------------------------------------

// CreateChannel 创建频道。名字重复（不区分大小写）返回 ErrConflict。
func (s *Store) CreateChannel(ctx context.Context, name string) (Channel, error) {
	ch := Channel{ID: newID(), Name: name, CreatedAt: time.Now().UnixMilli()}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO channels (id, name, created_at) VALUES (?, ?, ?)`,
		ch.ID, ch.Name, ch.CreatedAt)
	if isConstraint(err) {
		return Channel{}, ErrConflict
	}
	if err != nil {
		return Channel{}, fmt.Errorf("store: create channel: %w", err)
	}
	return ch, nil
}

// ListChannels 按创建时间列出全部频道。
func (s *Store) ListChannels(ctx context.Context) ([]Channel, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, name, created_at FROM channels ORDER BY created_at ASC, id ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list channels: %w", err)
	}
	defer rows.Close()

	channels := make([]Channel, 0, 8)
	for rows.Next() {
		var ch Channel
		if err := rows.Scan(&ch.ID, &ch.Name, &ch.CreatedAt); err != nil {
			return nil, fmt.Errorf("store: scan channel: %w", err)
		}
		channels = append(channels, ch)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate channels: %w", err)
	}
	return channels, nil
}

// ChannelByID 取单个频道；不存在返回 ErrNotFound。
func (s *Store) ChannelByID(ctx context.Context, id string) (Channel, error) {
	var ch Channel
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, created_at FROM channels WHERE id = ?`, id).
		Scan(&ch.ID, &ch.Name, &ch.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Channel{}, ErrNotFound
	}
	if err != nil {
		return Channel{}, fmt.Errorf("store: channel by id: %w", err)
	}
	return ch, nil
}

// ------------------------------------------------------------------
// messages
// ------------------------------------------------------------------

// maxSeqAttempts 是 seq 分配冲突时的重试次数。同进程内单连接池下正常不会冲突，
// 这里只是兜底（例如另一个进程/进程内两条通路同时写库）。
const maxSeqAttempts = 4

// AppendMessage 追加一条消息并分配每频道单调递增的 seq。
//
// seq 用单条 SQL 分配，保证「读 max + 写入」原子：
//
//	INSERT ... SELECT COALESCE(MAX(seq),0)+1 ... RETURNING seq
//
// 不用「先 SELECT 再 INSERT」的两步式写法，那在并发下会撞 UNIQUE(channel_id, seq)。
func (s *Store) AppendMessage(ctx context.Context, channelID, memberID, body string) (Message, error) {
	msg := Message{
		ID:        newID(),
		ChannelID: channelID,
		MemberID:  memberID,
		Body:      body,
		TS:        time.Now().UnixMilli(),
	}

	var lastErr error
	for attempt := 0; attempt < maxSeqAttempts; attempt++ {
		err := s.db.QueryRowContext(ctx,
			`INSERT INTO messages (id, channel_id, seq, member_id, body, ts)
			 SELECT ?, ?, COALESCE(MAX(seq), 0) + 1, ?, ?, ?
			   FROM messages WHERE channel_id = ?
			 RETURNING seq`,
			msg.ID, msg.ChannelID, msg.MemberID, msg.Body, msg.TS, msg.ChannelID).
			Scan(&msg.Seq)
		if err == nil {
			return msg, nil
		}
		if !isConstraint(err) {
			return Message{}, fmt.Errorf("store: append message: %w", err)
		}
		lastErr = err
	}
	return Message{}, fmt.Errorf("store: append message: seq contention: %w", lastErr)
}

// History 返回某频道 seq 升序的消息。beforeSeq > 0 时只返回 seq < beforeSeq 的，
// 用于向前翻页；limit <= 0 或超上限时按上限截断。
func (s *Store) History(ctx context.Context, channelID string, beforeSeq int64, limit int) ([]Message, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	var (
		rows *sql.Rows
		err  error
	)
	if beforeSeq > 0 {
		rows, err = s.db.QueryContext(ctx,
			`SELECT m.id, m.channel_id, m.seq, m.member_id, COALESCE(mb.name, ''), m.body, m.ts
			   FROM messages m LEFT JOIN members mb ON mb.id = m.member_id
			  WHERE m.channel_id = ? AND m.seq < ? ORDER BY m.seq DESC LIMIT ?`,
			channelID, beforeSeq, limit)
	} else {
		rows, err = s.db.QueryContext(ctx,
			`SELECT m.id, m.channel_id, m.seq, m.member_id, COALESCE(mb.name, ''), m.body, m.ts
			   FROM messages m LEFT JOIN members mb ON mb.id = m.member_id
			  WHERE m.channel_id = ? ORDER BY m.seq DESC LIMIT ?`,
			channelID, limit)
	}
	if err != nil {
		return nil, fmt.Errorf("store: history: %w", err)
	}
	defer rows.Close()

	// 倒序取、正序还：这样 LIMIT 截的是「最近的 N 条」，对外仍按 seq 升序。
	desc := make([]Message, 0, limit)
	for rows.Next() {
		var m Message
		if err := rows.Scan(&m.ID, &m.ChannelID, &m.Seq, &m.MemberID, &m.MemberName, &m.Body, &m.TS); err != nil {
			return nil, fmt.Errorf("store: scan message: %w", err)
		}
		desc = append(desc, m)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate messages: %w", err)
	}
	for i, j := 0, len(desc)-1; i < j; i, j = i+1, j-1 {
		desc[i], desc[j] = desc[j], desc[i]
	}
	return desc, nil
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func nullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}

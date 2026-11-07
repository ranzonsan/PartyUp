package store_test

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"

	"partyup/internal/store"
)

// TestDisplayNameMigrationBackfillsLegacyRows 锁定旧库升级路径：
//   - 旧 members 表没有 display_name；
//   - Open 时幂等 ADD COLUMN，并把旧行回填为登录名（旧行不会拿到空显示名）；
//   - 关库重开（等价服务端重启）仍保留显示名，第二次迁移无副作用；
//   - 新写入的显式 display_name 能持久化，未提供时回退登录名。
func TestDisplayNameMigrationBackfillsLegacyRows(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()
	path := filepath.Join(dir, "partyup.db")

	// 先造一个 v1 之前形态的库：只有 members 表，且没有 display_name 列。
	raw, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatalf("open legacy db: %v", err)
	}
	if _, err := raw.Exec(`CREATE TABLE members (
		id            TEXT PRIMARY KEY,
		name          TEXT NOT NULL COLLATE NOCASE UNIQUE,
		password_hash TEXT NOT NULL,
		is_admin      INTEGER NOT NULL DEFAULT 0,
		created_at    INTEGER NOT NULL
	)`); err != nil {
		t.Fatalf("create legacy members: %v", err)
	}
	if _, err := raw.Exec(
		`INSERT INTO members (id, name, password_hash, is_admin, created_at) VALUES (?, ?, ?, 0, ?)`,
		"legacy-alice", "alice", "hash-a", int64(100),
	); err != nil {
		t.Fatalf("insert legacy member: %v", err)
	}
	if err := raw.Close(); err != nil {
		t.Fatalf("close legacy db: %v", err)
	}

	// 第一次升级：加列 + 回填。
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("open store (migrate): %v", err)
	}
	rec, err := st.MemberByName(ctx, "alice")
	if err != nil {
		t.Fatalf("member by name after migrate: %v", err)
	}
	if rec.DisplayName != "alice" {
		t.Fatalf("legacy display_name = %q, want fallback alice", rec.DisplayName)
	}
	if err := st.Close(); err != nil {
		t.Fatalf("close migrated store: %v", err)
	}

	// 第二次打开（服务端重启）：迁移必须幂等，旧行仍可读。
	st2, err := store.Open(dir)
	if err != nil {
		t.Fatalf("reopen migrated store: %v", err)
	}
	rec2, err := st2.MemberByName(ctx, "alice")
	if err != nil {
		t.Fatalf("member by name after reopen: %v", err)
	}
	if rec2.DisplayName != "alice" {
		t.Fatalf("legacy display_name after reopen = %q, want alice", rec2.DisplayName)
	}

	// 新成员：显式显示名持久化；空显示名回退登录名。
	if _, err := st2.CreateMember(ctx, "bob", "Bobby", "hash-b", false); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := st2.CreateMember(ctx, "carol", "", "hash-c", false); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	members, err := st2.ListMembers(ctx)
	if err != nil {
		t.Fatalf("list members: %v", err)
	}
	if len(members) != 3 {
		t.Fatalf("member count = %d, want 3", len(members))
	}
	byName := map[string]string{}
	for _, m := range members {
		byName[m.Name] = m.DisplayName
	}
	if byName["bob"] != "Bobby" || byName["carol"] != "carol" || byName["alice"] != "alice" {
		t.Fatalf("display names = %v, want alice/alice, bob/Bobby, carol/carol", byName)
	}
	if err := st2.Close(); err != nil {
		t.Fatalf("close store2: %v", err)
	}

	// 第三次打开：新写入的显示名也要在重启后保留。
	st3, err := store.Open(dir)
	if err != nil {
		t.Fatalf("third open: %v", err)
	}
	defer st3.Close()
	rec3, err := st3.MemberByName(ctx, "bob")
	if err != nil {
		t.Fatalf("member by name bob after reopen: %v", err)
	}
	if rec3.DisplayName != "Bobby" {
		t.Fatalf("bob display_name after reopen = %q, want Bobby", rec3.DisplayName)
	}
}

package store_test

// 身份/权限第一批的持久层回归（2026-11-07）：
// 角色唯一性与旧库回填、所有权转让、封禁撤销会话、审计分页、频道/消息管理写操作。
// 全部用真实 SQLite（t.TempDir），不 mock——这些不变量靠 SQL 约束与事务保证。

import (
	"context"
	"errors"
	"testing"
	"time"

	"partyup/internal/auth"
	"partyup/internal/store"
)

// TestEnsureOwnerBackfill 覆盖旧库迁移：没有 owner 时把最早创建的成员回填为 owner，
// 重复执行幂等；回填后唯一索引生效（第二个 owner 写不进去）。
func TestEnsureOwnerBackfill(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()

	alice, err := st.CreateMember(ctx, "alice", "", "hash-a", true) // 旧库管理员位
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := st.CreateMember(ctx, "bob", "", "hash-b", false)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	name, err := st.EnsureOwner(ctx)
	if err != nil {
		t.Fatalf("ensure owner: %v", err)
	}
	if name != "alice" {
		t.Fatalf("fallback owner = %q, want alice", name)
	}
	aliceNow, err := st.MemberByID(ctx, alice.ID)
	if err != nil {
		t.Fatalf("reload alice: %v", err)
	}
	if aliceNow.Role != store.RoleOwner {
		t.Fatalf("alice role = %q, want owner", aliceNow.Role)
	}
	bobNow, err := st.MemberByID(ctx, bob.ID)
	if err != nil {
		t.Fatalf("reload bob: %v", err)
	}
	if bobNow.Role != store.RoleUser {
		t.Fatalf("bob role = %q, want user", bobNow.Role)
	}

	// 幂等：第二次不再回填。
	name, err = st.EnsureOwner(ctx)
	if err != nil || name != "" {
		t.Fatalf("second ensure owner = %q, %v; want empty, nil", name, err)
	}

	// 唯一 owner 索引：再建一个 owner 必须被约束拒绝（CreateMemberWithRole 映射 ErrConflict）。
	if _, err := st.CreateMemberWithRole(ctx, "carol", "", "hash-c", store.RoleOwner); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("second owner create err = %v, want ErrConflict", err)
	}
}

// TestTransferOwner 覆盖所有权转让：先降旧 owner 再升新 owner，任意时刻只有一个 owner。
func TestTransferOwner(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	alice, err := st.CreateMemberWithRole(ctx, "alice", "", "hash-a", store.RoleOwner)
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := st.CreateMemberWithRole(ctx, "bob", "", "hash-b", store.RoleUser)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	if err := st.TransferOwner(ctx, alice.ID, bob.ID); err != nil {
		t.Fatalf("transfer owner: %v", err)
	}
	aliceNow, _ := st.MemberByID(ctx, alice.ID)
	bobNow, _ := st.MemberByID(ctx, bob.ID)
	if aliceNow.Role != store.RoleAdmin {
		t.Fatalf("old owner role = %q, want admin", aliceNow.Role)
	}
	if bobNow.Role != store.RoleOwner {
		t.Fatalf("new owner role = %q, want owner", bobNow.Role)
	}

	// 转回去也要成立（证明索引没有被中间态卡住）。
	if err := st.TransferOwner(ctx, bob.ID, alice.ID); err != nil {
		t.Fatalf("transfer back: %v", err)
	}

	// 非 owner 不能转让：store 层兜底。
	if _, err := st.CreateMemberWithRole(ctx, "carol", "", "hash-c", store.RoleAdmin); err != nil {
		t.Fatalf("create carol: %v", err)
	}
}

// TestSetMemberRoleProtectsOwner：owner 不能被 role 接口降级（数据层兜底）。
func TestSetMemberRoleProtectsOwner(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	alice, _ := st.CreateMemberWithRole(ctx, "alice", "", "hash-a", store.RoleOwner)
	if err := st.SetMemberRole(ctx, alice.ID, store.RoleAdmin); !errors.Is(err, store.ErrOwnerProtected) {
		t.Fatalf("demote owner err = %v, want ErrOwnerProtected", err)
	}
	bob, _ := st.CreateMemberWithRole(ctx, "bob", "", "hash-b", store.RoleUser)
	if err := st.SetMemberRole(ctx, bob.ID, store.RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	bobNow, _ := st.MemberByID(ctx, bob.ID)
	if bobNow.Role != store.RoleAdmin || !bobNow.Admin {
		t.Fatalf("bob after promote = %+v, want admin", bobNow)
	}
	// is_admin 双写：升级后旧布尔位也为 true（降级兼容）。
	list, err := st.ListMembers(ctx)
	if err != nil {
		t.Fatalf("list members: %v", err)
	}
	for _, m := range list {
		if m.ID == bob.ID && !m.Admin {
			t.Fatal("bob.Admin should be true after promotion")
		}
	}
}

// TestBanRevokesSessionsAndProtectsOwner 覆盖封禁：撤销全部会话、幂等、owner 不可被封禁。
func TestBanRevokesSessionsAndProtectsOwner(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	alice, _ := st.CreateMemberWithRole(ctx, "alice", "", "hash-a", store.RoleOwner)
	bob, _ := st.CreateMember(ctx, "bob", "", "hash-b", false)

	token, err := auth.NewToken()
	if err != nil {
		t.Fatalf("new token: %v", err)
	}
	tokenHash := auth.HashToken(token)
	if err := st.CreateSession(ctx, bob.ID, tokenHash, time.Now().Add(time.Hour).UnixMilli()); err != nil {
		t.Fatalf("create session: %v", err)
	}

	if err := st.BanMember(ctx, bob.ID, alice.ID, "spam"); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	banned, err := st.IsBanned(ctx, bob.ID)
	if err != nil || !banned {
		t.Fatalf("is banned = %v, %v; want true", banned, err)
	}
	if _, err := st.MemberBySession(ctx, tokenHash); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("banned session still valid, err = %v", err)
	}

	// 重复封禁覆盖原因，仍然成功。
	if err := st.BanMember(ctx, bob.ID, alice.ID, "spam again"); err != nil {
		t.Fatalf("ban again: %v", err)
	}
	if err := st.BanMember(ctx, alice.ID, alice.ID, ""); !errors.Is(err, store.ErrOwnerProtected) {
		t.Fatalf("ban owner err = %v, want ErrOwnerProtected", err)
	}

	if err := st.UnbanMember(ctx, bob.ID); err != nil {
		t.Fatalf("unban: %v", err)
	}
	if banned, _ := st.IsBanned(ctx, bob.ID); banned {
		t.Fatal("bob still banned after unban")
	}
}

// TestAuditAppendAndPage 覆盖审计写入与倒序分页。
func TestAuditAppendAndPage(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	base := time.Now().UnixMilli()
	for i := 0; i < 5; i++ {
		err := st.AppendAudit(ctx, store.AuditEvent{
			AtMS: base + int64(i), ActorID: "actor", ActorName: "alice",
			Action: "member.kick", Target: "bob", Detail: `{"n":1}`, IP: "127.0.0.1",
		})
		if err != nil {
			t.Fatalf("append audit %d: %v", i, err)
		}
	}
	events, err := st.ListAudit(ctx, 2, 0)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	if len(events) != 2 {
		t.Fatalf("audit page len = %d, want 2", len(events))
	}
	if events[0].ID <= events[1].ID {
		t.Fatalf("audit not newest-first: %d, %d", events[0].ID, events[1].ID)
	}
	// before 翻页：只取更旧的。
	older, err := st.ListAudit(ctx, 10, events[1].ID)
	if err != nil {
		t.Fatalf("list audit before: %v", err)
	}
	for _, ev := range older {
		if ev.ID >= events[1].ID {
			t.Fatalf("before page returned id %d >= %d", ev.ID, events[1].ID)
		}
	}
	if len(older) != 3 {
		t.Fatalf("older page len = %d, want 3", len(older))
	}
}

// TestChannelAdminWrites 覆盖频道改名/说明/排序/删除。
func TestChannelAdminWrites(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	first, err := st.CreateChannel(ctx, "general")
	if err != nil {
		t.Fatalf("create general: %v", err)
	}
	second, err := st.CreateChannel(ctx, "random")
	if err != nil {
		t.Fatalf("create random: %v", err)
	}
	if first.Position >= second.Position {
		t.Fatalf("positions not increasing: %d, %d", first.Position, second.Position)
	}

	if err := st.RenameChannel(ctx, first.ID, "lobby"); err != nil {
		t.Fatalf("rename: %v", err)
	}
	if err := st.SetChannelDescription(ctx, first.ID, "welcome"); err != nil {
		t.Fatalf("set description: %v", err)
	}
	if err := st.ReorderChannels(ctx, []string{second.ID, first.ID}); err != nil {
		t.Fatalf("reorder: %v", err)
	}
	list, err := st.ListChannels(ctx)
	if err != nil {
		t.Fatalf("list channels: %v", err)
	}
	if len(list) != 2 || list[0].ID != second.ID || list[1].ID != first.ID {
		t.Fatalf("reordered list = %+v", list)
	}
	if list[1].Name != "lobby" || list[1].Description != "welcome" {
		t.Fatalf("channel after rename/desc = %+v", list[1])
	}
	if err := st.DeleteChannel(ctx, second.ID); err != nil {
		t.Fatalf("delete channel: %v", err)
	}
	if _, err := st.ChannelByID(ctx, second.ID); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("deleted channel lookup err = %v, want ErrNotFound", err)
	}
	if err := st.RenameChannel(ctx, "missing", "x"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("rename missing err = %v, want ErrNotFound", err)
	}
}

// TestDeleteMessage 覆盖管理删消息：返回原文供审计，跨频道不误删。
func TestDeleteMessage(t *testing.T) {
	st := openTestStore(t)
	ctx := context.Background()
	alice, _ := st.CreateMember(ctx, "alice", "", "hash-a", false)
	ch, err := st.CreateChannel(ctx, "general")
	if err != nil {
		t.Fatalf("create channel: %v", err)
	}
	other, err := st.CreateChannel(ctx, "other")
	if err != nil {
		t.Fatalf("create other channel: %v", err)
	}
	msg, err := st.AppendMessage(ctx, ch.ID, alice.ID, "hello")
	if err != nil {
		t.Fatalf("append message: %v", err)
	}
	if _, err := st.DeleteMessage(ctx, other.ID, msg.ID); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("cross-channel delete err = %v, want ErrNotFound", err)
	}
	deleted, err := st.DeleteMessage(ctx, ch.ID, msg.ID)
	if err != nil {
		t.Fatalf("delete message: %v", err)
	}
	if deleted.Body != "hello" || deleted.MemberID != alice.ID {
		t.Fatalf("deleted message = %+v", deleted)
	}
	history, err := st.History(ctx, ch.ID, 0, 10)
	if err != nil {
		t.Fatalf("history: %v", err)
	}
	if len(history) != 0 {
		t.Fatalf("history after delete = %d messages, want 0", len(history))
	}
}

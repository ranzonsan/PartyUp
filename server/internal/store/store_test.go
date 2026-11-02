package store_test

import (
	"context"
	"errors"
	"sync"
	"testing"

	"partyup/internal/store"
)

func openTestStore(t *testing.T) *store.Store {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })
	return st
}

func TestMemberAndSessionLifecycle(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	m, err := st.CreateMember(ctx, "alice", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}
	if m.ID == "" || !m.Admin {
		t.Fatalf("member = %+v, want non-empty id and admin", m)
	}

	// 名字唯一（不区分大小写）。
	if _, err := st.CreateMember(ctx, "ALICE", "hash-b", false); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("duplicate member err = %v, want ErrConflict", err)
	}

	rec, err := st.MemberByName(ctx, "Alice")
	if err != nil {
		t.Fatalf("member by name: %v", err)
	}
	if rec.ID != m.ID || rec.PasswordHash != "hash-a" {
		t.Fatalf("member record = %+v", rec)
	}

	// 会话：建 → 查 → 撤销 → 查不到。
	if err := st.CreateSession(ctx, m.ID, "session-hash-1"); err != nil {
		t.Fatalf("create session: %v", err)
	}
	got, err := st.MemberBySession(ctx, "session-hash-1")
	if err != nil {
		t.Fatalf("member by session: %v", err)
	}
	if got.ID != m.ID {
		t.Fatalf("session member = %s, want %s", got.ID, m.ID)
	}
	if err := st.RevokeSession(ctx, "session-hash-1"); err != nil {
		t.Fatalf("revoke session: %v", err)
	}
	if _, err := st.MemberBySession(ctx, "session-hash-1"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("revoked session err = %v, want ErrNotFound", err)
	}
}

func TestRegisterWithInvite(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	if _, err := st.CreateInvite(ctx, "INVITE0001", ""); err != nil {
		t.Fatalf("create invite: %v", err)
	}

	m, err := st.RegisterWithInvite(ctx, "INVITE0001", "bob", "hash-b")
	if err != nil {
		t.Fatalf("register with invite: %v", err)
	}
	if m.Name != "bob" || m.Admin {
		t.Fatalf("registered member = %+v, want bob non-admin", m)
	}

	// 复用同一个码 → ErrInviteUsed。
	if _, err := st.RegisterWithInvite(ctx, "INVITE0001", "carol", "hash-c"); !errors.Is(err, store.ErrInviteUsed) {
		t.Fatalf("reused invite err = %v, want ErrInviteUsed", err)
	}

	// 未知码 → ErrNotFound。
	if _, err := st.RegisterWithInvite(ctx, "NOPE000001", "dave", "hash-d"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("unknown invite err = %v, want ErrNotFound", err)
	}

	// 名字撞车 → ErrNameTaken，且不能把新邀请码烧掉。
	if _, err := st.CreateInvite(ctx, "INVITE0002", ""); err != nil {
		t.Fatalf("create second invite: %v", err)
	}
	if _, err := st.RegisterWithInvite(ctx, "INVITE0002", "bob", "hash-e"); !errors.Is(err, store.ErrNameTaken) {
		t.Fatalf("name clash err = %v, want ErrNameTaken", err)
	}
	inv, err := st.InviteByCode(ctx, "INVITE0002")
	if err != nil {
		t.Fatalf("invite by code: %v", err)
	}
	if inv.UsedBy != "" {
		t.Fatalf("invite was consumed by a failed registration (used_by=%q)", inv.UsedBy)
	}
}

func TestAppendMessageSeqMonotonic(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	ch, err := st.CreateChannel(ctx, "general")
	if err != nil {
		t.Fatalf("create channel: %v", err)
	}
	m, err := st.CreateMember(ctx, "alice", "hash", false)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}

	const writers, perWriter = 4, 5
	var wg sync.WaitGroup
	errs := make(chan error, writers*perWriter)
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func(writer int) {
			defer wg.Done()
			for i := 0; i < perWriter; i++ {
				if _, err := st.AppendMessage(ctx, ch.ID, m.ID, "msg"); err != nil {
					errs <- err
					return
				}
			}
		}(w)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("append message: %v", err)
	}

	msgs, err := st.History(ctx, ch.ID, 0, 100)
	if err != nil {
		t.Fatalf("history: %v", err)
	}
	if len(msgs) != writers*perWriter {
		t.Fatalf("history size = %d, want %d", len(msgs), writers*perWriter)
	}
	for i, msg := range msgs {
		if msg.Seq != int64(i+1) {
			t.Fatalf("message[%d].seq = %d, want %d", i, msg.Seq, i+1)
		}
	}
}

func TestHistoryPaginationAndPersistence(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()

	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	ch, err := st.CreateChannel(ctx, "general")
	if err != nil {
		t.Fatalf("create channel: %v", err)
	}
	m, err := st.CreateMember(ctx, "alice", "hash", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}
	for i := 0; i < 6; i++ {
		if _, err := st.AppendMessage(ctx, ch.ID, m.ID, "msg"); err != nil {
			t.Fatalf("append: %v", err)
		}
	}

	recent, err := st.History(ctx, ch.ID, 0, 2)
	if err != nil {
		t.Fatalf("history recent: %v", err)
	}
	if len(recent) != 2 || recent[0].Seq != 5 || recent[1].Seq != 6 {
		t.Fatalf("recent = %+v, want seq 5,6", recent)
	}

	older, err := st.History(ctx, ch.ID, 5, 10)
	if err != nil {
		t.Fatalf("history older: %v", err)
	}
	if len(older) != 4 || older[0].Seq != 1 || older[3].Seq != 4 {
		t.Fatalf("older = %+v, want seq 1..4", older)
	}

	// 关库重开：成员、频道、消息都还在。
	if err := st.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	st2, err := store.Open(dir)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	defer st2.Close()

	count, err := st2.MemberCount(ctx)
	if err != nil || count != 1 {
		t.Fatalf("member count after reopen = %d err=%v, want 1", count, err)
	}
	channels, err := st2.ListChannels(ctx)
	if err != nil || len(channels) != 1 {
		t.Fatalf("channels after reopen = %v err=%v, want 1", channels, err)
	}
	msgs, err := st2.History(ctx, ch.ID, 0, 100)
	if err != nil || len(msgs) != 6 {
		t.Fatalf("messages after reopen = %d err=%v, want 6", len(msgs), err)
	}
}

func TestSettings(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	if _, err := st.GetSetting(ctx, "missing"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("missing setting err = %v, want ErrNotFound", err)
	}
	if err := st.SetSetting(ctx, "k", "v1"); err != nil {
		t.Fatalf("set setting: %v", err)
	}
	if got, err := st.GetSetting(ctx, "k"); err != nil || got != "v1" {
		t.Fatalf("get setting = %q err=%v", got, err)
	}
	if err := st.SetSetting(ctx, "k", "v2"); err != nil {
		t.Fatalf("overwrite setting: %v", err)
	}
	if got, _ := st.GetSetting(ctx, "k"); got != "v2" {
		t.Fatalf("overwritten setting = %q, want v2", got)
	}
	if err := st.DeleteSetting(ctx, "k"); err != nil {
		t.Fatalf("delete setting: %v", err)
	}
	if _, err := st.GetSetting(ctx, "k"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("deleted setting err = %v, want ErrNotFound", err)
	}
}

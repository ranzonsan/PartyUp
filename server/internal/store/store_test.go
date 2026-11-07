package store_test

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

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

	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}
	if m.ID == "" || !m.Admin {
		t.Fatalf("member = %+v, want non-empty id and admin", m)
	}

	// 名字唯一（不区分大小写）。
	if _, err := st.CreateMember(ctx, "ALICE", "", "hash-b", false); !errors.Is(err, store.ErrConflict) {
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
	if err := st.CreateSession(ctx, m.ID, "session-hash-1",
		time.Now().Add(store.DefaultSessionTTL).UnixMilli()); err != nil {
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
	// 撤销幂等：重复撤销不报错（POST /api/logout 重复调用要能继续 204）。
	if err := st.RevokeSession(ctx, "session-hash-1"); err != nil {
		t.Fatalf("second revoke err = %v, want nil (idempotent)", err)
	}
	// 完全未知的 token → ErrNotFound（上层映射 401）。
	if err := st.RevokeSession(ctx, "never-issued"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("unknown token revoke err = %v, want ErrNotFound", err)
	}
}

func TestRegisterWithInvite(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	if _, err := st.CreateInvite(ctx, "INVITE0001", ""); err != nil {
		t.Fatalf("create invite: %v", err)
	}

	m, err := st.RegisterWithInvite(ctx, "INVITE0001", "bob", "", "hash-b")
	if err != nil {
		t.Fatalf("register with invite: %v", err)
	}
	if m.Name != "bob" || m.Admin {
		t.Fatalf("registered member = %+v, want bob non-admin", m)
	}

	// 复用同一个码 → ErrInviteUsed。
	if _, err := st.RegisterWithInvite(ctx, "INVITE0001", "carol", "", "hash-c"); !errors.Is(err, store.ErrInviteUsed) {
		t.Fatalf("reused invite err = %v, want ErrInviteUsed", err)
	}

	// 未知码 → ErrNotFound。
	if _, err := st.RegisterWithInvite(ctx, "NOPE000001", "dave", "", "hash-d"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("unknown invite err = %v, want ErrNotFound", err)
	}

	// 名字撞车 → ErrNameTaken，且不能把新邀请码烧掉。
	if _, err := st.CreateInvite(ctx, "INVITE0002", ""); err != nil {
		t.Fatalf("create second invite: %v", err)
	}
	if _, err := st.RegisterWithInvite(ctx, "INVITE0002", "bob", "", "hash-e"); !errors.Is(err, store.ErrNameTaken) {
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
	m, err := st.CreateMember(ctx, "alice", "", "hash", false)
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
	m, err := st.CreateMember(ctx, "alice", "", "hash", true)
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

// ------------------------------------------------------------------
// 会话撤销 / 过期 / 单端登录（2026-11-07）
// ------------------------------------------------------------------

// TestSessionExpiry 覆盖 B 的读路径：expires_at 已过的 token 不能换出成员。
func TestSessionExpiry(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)

	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}

	// 已过期（1 秒前）。MemberBySession 必须返回 ErrNotFound。
	if err := st.CreateSession(ctx, m.ID, "expired-hash",
		time.Now().Add(-time.Second).UnixMilli()); err != nil {
		t.Fatalf("create expired session: %v", err)
	}
	if _, err := st.MemberBySession(ctx, "expired-hash"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("expired session err = %v, want ErrNotFound", err)
	}
	// 对照：同一成员的有效会话仍可用（过期只影响那一行）。
	if err := st.CreateSession(ctx, m.ID, "valid-hash",
		time.Now().Add(time.Minute).UnixMilli()); err != nil {
		t.Fatalf("create valid session: %v", err)
	}
	if _, err := st.MemberBySession(ctx, "valid-hash"); err != nil {
		t.Fatalf("valid session err = %v, want nil", err)
	}
}

// TestReplaceSessionsRevokesOthers 覆盖 C 的顺序语义：替换后旧 token 立即失效，
// 新 token 有效，返回值为被撤销条数。
func TestReplaceSessionsRevokesOthers(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)
	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}
	expires := time.Now().Add(time.Hour).UnixMilli()

	// 第一次登录：没有旧会话可撤销。
	revoked, err := st.ReplaceSessions(ctx, m.ID, "token-hash-1", expires)
	if err != nil {
		t.Fatalf("replace sessions (first): %v", err)
	}
	if revoked != 0 {
		t.Fatalf("revoked on first login = %d, want 0", revoked)
	}

	// 第二次登录：撤销第 1 条，签发第 2 条。
	revoked, err = st.ReplaceSessions(ctx, m.ID, "token-hash-2", expires)
	if err != nil {
		t.Fatalf("replace sessions (second): %v", err)
	}
	if revoked != 1 {
		t.Fatalf("revoked on second login = %d, want 1", revoked)
	}
	if _, err := st.MemberBySession(ctx, "token-hash-1"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("old session err = %v, want ErrNotFound", err)
	}
	if _, err := st.MemberBySession(ctx, "token-hash-2"); err != nil {
		t.Fatalf("new session err = %v, want nil", err)
	}

	// 不同成员互不影响：bob 的会话不能被 alice 的登录撤销。
	bob, err := st.CreateMember(ctx, "bob", "", "hash-b", false)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := st.ReplaceSessions(ctx, bob.ID, "bob-hash", expires); err != nil {
		t.Fatalf("replace bob sessions: %v", err)
	}
	if _, err := st.ReplaceSessions(ctx, m.ID, "token-hash-3", expires); err != nil {
		t.Fatalf("replace alice sessions again: %v", err)
	}
	if _, err := st.MemberBySession(ctx, "bob-hash"); err != nil {
		t.Fatalf("bob session err = %v, want nil (other member must be untouched)", err)
	}
}

// TestReplaceSessionsConcurrent 是 C 的并发验收：多个 goroutine 同时登录同一账号，
// 最终**恰好只有一条**有效会话。
//
// 关键不是「最后谁赢」（顺序未定义），而是「不能两条都有效」——那正是先撤销后
// 插入分成两步时会出现的单端登录击穿。
func TestReplaceSessionsConcurrent(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)
	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}

	const logins = 8
	// 稳定且互不相同的 token hash：用索引字母后缀（真实 token hash 是 hex）。
	hashes := make([]string, logins)
	for i := range hashes {
		hashes[i] = "concurrent-hash-" + string(rune('a'+i))
	}

	expires := time.Now().Add(time.Hour).UnixMilli()
	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := 0; i < logins; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start // 尽量让 8 个登录同时进入
			if _, err := st.ReplaceSessions(ctx, m.ID, hashes[i], expires); err != nil {
				t.Errorf("concurrent replace %d: %v", i, err)
			}
		}(i)
	}
	close(start)
	wg.Wait()

	active := 0
	for _, h := range hashes {
		_, err := st.MemberBySession(ctx, h)
		switch {
		case err == nil:
			active++
		case errors.Is(err, store.ErrNotFound):
		default:
			t.Fatalf("member by session %q: %v", h, err)
		}
	}
	if active != 1 {
		t.Fatalf("active sessions after %d concurrent logins = %d, want exactly 1", logins, active)
	}
}

// TestTouchSessionSlidingRenewal 覆盖 B 的滑动续期与 5 分钟写节流（确定性时钟注入：
// TouchSession 的 now 是参数，不需要 sleep）。
func TestTouchSessionSlidingRenewal(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)
	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}

	const ttl = 30 * 24 * time.Hour
	const granularity = 5 * time.Minute
	now := time.Now().UnixMilli()
	initial := now + ttl.Milliseconds()
	if err := st.CreateSession(ctx, m.ID, "sliding-hash", initial); err != nil {
		t.Fatalf("create session: %v", err)
	}

	// 1) 刚签发：剩余 = TTL，未超过节流窗口 → 不写。
	if err := st.TouchSession(ctx, "sliding-hash", now, ttl, granularity); err != nil {
		t.Fatalf("touch (no-op): %v", err)
	}
	state, err := st.SessionState(ctx, "sliding-hash")
	if err != nil {
		t.Fatalf("session state: %v", err)
	}
	if state.ExpiresAt != initial {
		t.Fatalf("expires_at after immediate touch = %d, want unchanged %d", state.ExpiresAt, initial)
	}

	// 2) 模拟 10 分钟后命中：剩余比 TTL 少 10 分钟 > 粒度 5 分钟 → 写，推到 now2+TTL。
	now2 := now + 10*time.Minute.Milliseconds()
	if err := st.TouchSession(ctx, "sliding-hash", now2, ttl, granularity); err != nil {
		t.Fatalf("touch (renew): %v", err)
	}
	state, err = st.SessionState(ctx, "sliding-hash")
	if err != nil {
		t.Fatalf("session state after renew: %v", err)
	}
	want := now2 + ttl.Milliseconds()
	if state.ExpiresAt != want {
		t.Fatalf("expires_at after renew = %d, want %d", state.ExpiresAt, want)
	}

	// 3) 已撤销的会话不会被续期「复活」：TouchSession 的 WHERE 带 revoked_at IS NULL。
	if err := st.RevokeSession(ctx, "sliding-hash"); err != nil {
		t.Fatalf("revoke: %v", err)
	}
	now3 := now2 + 20*time.Minute.Milliseconds()
	if err := st.TouchSession(ctx, "sliding-hash", now3, ttl, granularity); err != nil {
		t.Fatalf("touch revoked: %v", err)
	}
	state, err = st.SessionState(ctx, "sliding-hash")
	if err != nil {
		t.Fatalf("session state after revoked touch: %v", err)
	}
	if state.ExpiresAt != want {
		t.Fatalf("expires_at after touching revoked session = %d, want unchanged %d", state.ExpiresAt, want)
	}
	if _, err := st.MemberBySession(ctx, "sliding-hash"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("revoked session err = %v, want ErrNotFound", err)
	}
}

// TestSessionState 覆盖 WS 鉴权失败时的错误码判据：撤销、过期、未知必须能区分。
func TestSessionState(t *testing.T) {
	ctx := context.Background()
	st := openTestStore(t)
	m, err := st.CreateMember(ctx, "alice", "", "hash-a", true)
	if err != nil {
		t.Fatalf("create member: %v", err)
	}

	// 未知 token。
	state, err := st.SessionState(ctx, "unknown-hash")
	if err != nil {
		t.Fatalf("session state (unknown): %v", err)
	}
	if state.Exists || state.Revoked || state.Expired {
		t.Fatalf("unknown state = %+v, want all false", state)
	}

	// 已撤销。
	if err := st.CreateSession(ctx, m.ID, "revoked-hash",
		time.Now().Add(time.Hour).UnixMilli()); err != nil {
		t.Fatalf("create session: %v", err)
	}
	if err := st.RevokeSession(ctx, "revoked-hash"); err != nil {
		t.Fatalf("revoke: %v", err)
	}
	state, err = st.SessionState(ctx, "revoked-hash")
	if err != nil {
		t.Fatalf("session state (revoked): %v", err)
	}
	if !state.Exists || !state.Revoked || state.Expired {
		t.Fatalf("revoked state = %+v, want Exists+Revoked", state)
	}

	// 已过期（未撤销）。
	if err := st.CreateSession(ctx, m.ID, "expired-hash",
		time.Now().Add(-time.Second).UnixMilli()); err != nil {
		t.Fatalf("create expired session: %v", err)
	}
	state, err = st.SessionState(ctx, "expired-hash")
	if err != nil {
		t.Fatalf("session state (expired): %v", err)
	}
	if !state.Exists || state.Revoked || !state.Expired {
		t.Fatalf("expired state = %+v, want Exists+Expired", state)
	}
}

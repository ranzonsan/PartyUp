package server

// 内部测试（不是 server_test）：需要注入 memberStatePublishHook，把两次并发的
// 名册发布确定性地交错，证明「旧快照不会在新快照之后发送」。
//
// 回归的故障形态（A2）：
//   T1 在 h.mu 内取到 S1（bob 离线）后锁外查 DB；
//   T2 取到 S2（bob 在线）并先发送；
//   T1 随后把 S1 发出去 —— 客户端最终看到 bob 离线。
// 仅凭 -race 无法发现（没有数据竞争，只有发布顺序错乱），所以这里用注入点
// 在「快照已完成、尚未发送」处暂停 T1，再启动 T2，直接断言发送顺序。

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
	"partyup/internal/store"
)

func newPublishTestHub(t *testing.T) *Hub {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	return NewHub(st, log, "test", "0", "relay", "relay", store.DefaultSessionTTL, nil, sfu.ICEConfig{})
}

// fakeRosterConn 是一个只实现发布路径所需字段的连接：authed + member + 带缓冲的 send。
func fakeRosterConn(id, name string) *conn {
	return &conn{
		member: protocol.Member{ID: id, Name: name, DisplayName: name},
		authed: true,
		send:   make(chan outbound, 32),
		done:   make(chan struct{}),
	}
}

// recvRoster 从 fake 连接的发送队列取一条 members.state（跳过其它类型）。
func recvRoster(t *testing.T, c *conn, timeout time.Duration) []protocol.MemberRosterEntry {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatal("timed out waiting for members.state")
		}
		select {
		case out := <-c.send:
			env, err := protocol.Decode(out.data)
			if err != nil {
				t.Fatalf("decode pushed envelope: %v", err)
			}
			if env.Type != protocol.TypeMembersState {
				continue
			}
			var p protocol.MemberListOK
			if err := env.DecodePayload(&p); err != nil {
				t.Fatalf("decode members.state: %v", err)
			}
			return p.Members
		case <-time.After(remaining):
			t.Fatal("timed out waiting for members.state")
		}
	}
}

func rosterOnline(members []protocol.MemberRosterEntry, id string) (bool, bool) {
	for _, m := range members {
		if m.ID == id {
			return m.Online, true
		}
	}
	return false, false
}

func TestMemberStatePublishKeepsSnapshotOrder(t *testing.T) {
	h := newPublishTestHub(t)
	ctx := context.Background()
	alice, err := h.store.CreateMember(ctx, "alice", "alice", "hash", false)
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := h.store.CreateMember(ctx, "bob", "bob", "hash", false)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	aliceConn := fakeRosterConn(alice.ID, alice.Name)
	h.mu.Lock()
	h.conns[aliceConn] = struct{}{}
	h.mu.Unlock()

	firstSnapshot := make(chan uint64, 1)
	releaseFirst := make(chan struct{})
	h.memberStatePublishHook = func(seq uint64) {
		if seq == 1 {
			firstSnapshot <- seq
			<-releaseFirst
		}
	}

	// T1：快照 S1（此时 bob 还没连上来），在发送前被 hook 暂停。
	done1 := make(chan struct{})
	go func() {
		h.broadcastMemberState()
		close(done1)
	}()
	select {
	case <-firstSnapshot:
	case <-time.After(5 * time.Second):
		t.Fatal("first publish never reached the snapshot hook")
	}

	// bob 上线（新的连接进入 h.conns）→ 触发 T2 发布。
	bobConn := fakeRosterConn(bob.ID, bob.Name)
	h.mu.Lock()
	h.conns[bobConn] = struct{}{}
	h.mu.Unlock()
	done2 := make(chan struct{})
	go func() {
		h.broadcastMemberState()
		close(done2)
	}()

	// 关键断言：T1 还没发送时，发送队列里不能先出现 T2 的新快照。
	// 若发布未串行化，T2 会在这里先发出 bob=online，随后 T1 的旧快照覆盖它。
	select {
	case out := <-aliceConn.send:
		t.Fatalf("a newer snapshot was sent while an older snapshot was still pending: %s", out.data)
	case <-time.After(150 * time.Millisecond):
		// 正常：T2 被发布序列挡住，必须等 T1 先发。
	}

	close(releaseFirst)
	select {
	case <-done1:
	case <-time.After(5 * time.Second):
		t.Fatal("first publish did not finish")
	}
	select {
	case <-done2:
	case <-time.After(5 * time.Second):
		t.Fatal("second publish did not finish")
	}

	// 发送顺序必须与快照顺序一致：S1（bob 离线）→ S2（bob 在线），
	// 客户端最后停在 S2，不会出现「新状态之后又收到旧状态」。
	first := recvRoster(t, aliceConn, 2*time.Second)
	if online, ok := rosterOnline(first, bob.ID); !ok || online {
		t.Fatalf("first published roster has bob online=%v (ok=%v), want offline snapshot first", online, ok)
	}
	second := recvRoster(t, aliceConn, 2*time.Second)
	if online, ok := rosterOnline(second, bob.ID); !ok || !online {
		t.Fatalf("second published roster has bob online=%v (ok=%v), want the newer snapshot last", online, ok)
	}

	h.memberStatePublishMu.Lock()
	seq := h.memberStatePublishSeq
	h.memberStatePublishMu.Unlock()
	if seq != 2 {
		t.Fatalf("publish sequence = %d, want 2", seq)
	}
}

// TestMemberStatePublishSerializedUnderConcurrency 是同一不变量的压力版：
// 多个 goroutine 并发触发发布，所有推送必须按发布序号单调、且最后一个快照是
// 最终状态（bob 在线）。它不能替代上面的确定性交错测试，但能覆盖真实调用路径。
func TestMemberStatePublishSerializedUnderConcurrency(t *testing.T) {
	h := newPublishTestHub(t)
	ctx := context.Background()
	alice, err := h.store.CreateMember(ctx, "alice", "alice", "hash", false)
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := h.store.CreateMember(ctx, "bob", "bob", "hash", false)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	aliceConn := fakeRosterConn(alice.ID, alice.Name)
	h.mu.Lock()
	h.conns[aliceConn] = struct{}{}
	h.mu.Unlock()

	var seen []uint64
	h.memberStatePublishHook = func(seq uint64) { seen = append(seen, seq) }

	start := make(chan struct{})
	const workers = 8
	done := make(chan struct{}, workers)
	for i := 0; i < workers; i++ {
		go func() {
			<-start
			h.broadcastMemberState()
			done <- struct{}{}
		}()
	}
	bobConn := fakeRosterConn(bob.ID, bob.Name)
	h.mu.Lock()
	h.conns[bobConn] = struct{}{}
	h.mu.Unlock()
	close(start)
	for i := 0; i < workers; i++ {
		<-done
	}
	for i, seq := range seen {
		if seq != uint64(i+1) {
			t.Fatalf("publish hook order = %v, want 1..%d", seen, workers)
		}
	}
	// 每个连接最终都应收到 bob=online 的快照（最后一条）。
	last := recvRoster(t, aliceConn, 2*time.Second)
	if online, ok := rosterOnline(last, bob.ID); !ok || !online {
		t.Fatalf("first queued roster has bob online=%v (ok=%v), want online", online, ok)
	}
}

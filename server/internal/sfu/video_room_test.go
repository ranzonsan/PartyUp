package sfu

// 视频房间的并发/生命周期与「一次一路」绑定合同。
//
// 这些测试不建真实 RTP 流：Join 只做 SDP/ICE（host 候选，不发网络包），
// Watch 绑定用本地静态轨验证槽位分配。真实的帧转发由媒体核心/端到端验收覆盖。

import (
	"fmt"
	"io"
	"log/slog"
	"sync"
	"testing"

	"github.com/pion/webrtc/v4"
)

func newTestVideoRoom(t *testing.T) *VideoRoom {
	t.Helper()
	room, err := NewVideo(
		"test-channel",
		slog.New(slog.NewTextHandler(io.Discard, nil)),
		func(string, string, string) {},
		nil,
	)
	if err != nil {
		t.Fatalf("NewVideo: %v", err)
	}
	t.Cleanup(room.Close)
	return room
}

func addFakePublication(t *testing.T, room *VideoRoom, owner string, slot int) *webrtc.TrackLocalStaticRTP {
	t.Helper()
	track, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
		MimeType:  webrtc.MimeTypeH264,
		ClockRate: 90000,
	}, "screen-"+owner, "partyup-relay-video")
	if err != nil {
		t.Fatalf("new fake track: %v", err)
	}
	room.mu.Lock()
	room.pubs[owner] = &videoPublication{owner: owner, track: track}
	room.slotOwners[slot] = owner
	room.mu.Unlock()
	return track
}

// TestVideoRoomJoinCloseConcurrent 是对 -race 报告的回归：
// Join 在旧实现里先发布 peer、锁外创建 PC 并回写 p.pc；Close 并发读到 p.pc 时
// 既可能是 nil（漏关 PC）也可能与写竞争。现在 Join/Close 串行化且 p.pc 发布后不可变。
func TestVideoRoomJoinCloseConcurrent(t *testing.T) {
	room := newTestVideoRoom(t)

	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		member := fmt.Sprintf("member-%d", i)
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = room.JoinWithMode(member, i%2 == 0)
		}()
		wg.Add(1)
		go func() {
			defer wg.Done()
			room.Close()
		}()
	}
	wg.Wait()

	// 房间已关闭：之后任何 Join 都必须明确失败，不能留下半建的 PC/peer。
	if err := room.JoinWithMode("late", true); err == nil {
		t.Fatal("JoinWithMode after concurrent Close returned nil, want closed error")
	}
	if got := room.Members(); len(got) != 0 {
		t.Fatalf("Members after Close = %v, want empty", got)
	}
	room.Close() // repeated Close must remain safe and idempotent
}

// TestVideoRoomWatchSwitchesTargetAndSlot 锁定一次一路的核心不变量：
//   - watch 换目标先解绑旧目标（不会同时绑两路）；
//   - 新目标落在与上一目标不同的槽（0↔1），供原生按 track 区分新旧目标；
//   - 重复 watch 同一目标幂等（不换槽，不中断画面）；
//   - unwatch 清空绑定并停止转发；StopShare 撤掉观看者目标，重新共享必须重新 watch。
func TestVideoRoomWatchSwitchesTargetAndSlot(t *testing.T) {
	room := newTestVideoRoom(t)
	for _, member := range []string{"sharer-a", "sharer-b", "viewer"} {
		if err := room.JoinWithMode(member, member == "viewer"); err != nil {
			t.Fatalf("join %s: %v", member, err)
		}
	}
	addFakePublication(t, room, "sharer-a", 0)
	addFakePublication(t, room, "sharer-b", 1)

	slotA, err := room.Watch("viewer", "sharer-a")
	if err != nil {
		t.Fatalf("watch a: %v", err)
	}
	if slotA != 0 {
		t.Fatalf("first watch slot = %d, want 0", slotA)
	}
	if bound := room.Bound("viewer"); len(bound) != 1 || bound[0] != "sharer-a" {
		t.Fatalf("bound after watch a = %v, want [sharer-a]", bound)
	}

	// 同一目标重复 watch：槽位不变（幂等）。
	again, err := room.Watch("viewer", "sharer-a")
	if err != nil || again != slotA {
		t.Fatalf("idempotent watch slot = %d (err %v), want %d", again, err, slotA)
	}

	// 换目标：旧目标必须解绑，新目标占用另一个槽。
	slotB, err := room.Watch("viewer", "sharer-b")
	if err != nil {
		t.Fatalf("watch b: %v", err)
	}
	if slotB == slotA {
		t.Fatalf("switch kept slot %d; new target must use a different video slot", slotB)
	}
	if bound := room.Bound("viewer"); len(bound) != 1 || bound[0] != "sharer-b" {
		t.Fatalf("bound after switch = %v, want only [sharer-b]", bound)
	}

	// 切回：槽位再次交替。
	back, err := room.Watch("viewer", "sharer-a")
	if err != nil || back != slotA {
		t.Fatalf("switch-back slot = %d (err %v), want %d", back, err, slotA)
	}

	// 停发：绑定与 target 同时清空；重新发布后不会自动续看。
	room.StopShare("sharer-a")
	if target, slot, manual, ok := room.WatchState("viewer"); !ok || target != "" || slot != -1 || !manual {
		t.Fatalf("watch state after StopShare = target=%q slot=%d manual=%v ok=%v, want cleared manual",
			target, slot, manual, ok)
	}
	if bound := room.Bound("viewer"); len(bound) != 0 {
		t.Fatalf("bound after StopShare = %v, want empty", bound)
	}

	// unwatch：幂等，保持 manual（不能退回自动观看）。
	room.Unwatch("viewer")
	room.Unwatch("viewer")
	if target, slot, manual, ok := room.WatchState("viewer"); !ok || target != "" || slot != -1 || !manual {
		t.Fatalf("watch state after unwatch = target=%q slot=%d manual=%v ok=%v", target, slot, manual, ok)
	}
	if bound := room.Bound("viewer"); len(bound) != 0 {
		t.Fatalf("bound after unwatch = %v, want empty", bound)
	}
}

// TestVideoRoomLegacyViewerAutoBinds 锁定旧客户端兼容：manual=false 的观看者
// 不需要 watch 就绑定已有共享（v1 行为），而 manual 观看者未点不接收。
func TestVideoRoomLegacyViewerAutoBinds(t *testing.T) {
	room := newTestVideoRoom(t)
	if err := room.JoinWithMode("sharer", false); err != nil {
		t.Fatalf("join sharer: %v", err)
	}
	addFakePublication(t, room, "sharer", 0)

	// legacy 观看者在已有共享之后加入：joinLocked 必须自动绑定（v1 行为）。
	if err := room.JoinWithMode("legacy", false); err != nil {
		t.Fatalf("join legacy: %v", err)
	}
	if bound := room.Bound("legacy"); len(bound) != 1 || bound[0] != "sharer" {
		t.Fatalf("legacy bound = %v, want [sharer] (automatic viewing)", bound)
	}

	// manual 观看者即使共享已存在，未 watch 前也不得绑定（未点不接收）。
	if err := room.JoinWithMode("manual", true); err != nil {
		t.Fatalf("join manual: %v", err)
	}
	if bound := room.Bound("manual"); len(bound) != 0 {
		t.Fatalf("manual viewer bound without watch = %v, want empty", bound)
	}
	if target, slot, manual, ok := room.WatchState("manual"); !ok || target != "" || slot != -1 || !manual {
		t.Fatalf("manual watch state = target=%q slot=%d manual=%v ok=%v", target, slot, manual, ok)
	}
}

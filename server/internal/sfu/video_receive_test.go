package sfu

// B2 严格接收会话（独立 receive-only PC）的回归。
//
// 这里验证的是真实代码路径的**传输身份与会话归属**：
//   - 每次 watch 都新建一条只含 sendonly video 的 PC：新 ICE ufrag/DTLS 指纹；
//   - session/nonce 绑定观看者，旧会话的信令在关闭后被拒绝；
//   - 关闭接收会话不改变发布状态（pubs 仍存在、发布轨仍可用）；
//   - RTP 只写入当前会话自己的 TrackLocalStaticRTP，关闭后的会话计数不再增长。
//
// 没有做 UDP/SRTP 旧包的字面延迟重放（见交付说明），因此 RTP 级隔离仍由
// 客户端原生回归（真实 loopback DTLS/SRTP）覆盖旧发布者继续发包、新会话 0 帧。

import (
	"io"
	"log/slog"
	"testing"

	"github.com/pion/rtp"
	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

func newReceiveTestRoom(t *testing.T) *VideoRoom {
	t.Helper()
	room, err := NewVideo(
		"receive-test-channel",
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

// sdpTransportIdentity 用 pion/sdp 解析 offer 的 ICE ufrag 与 DTLS 指纹，
// 不手写 SDP 字符串解析。新的接收传输必须两项都不同。
func sdpTransportIdentity(t *testing.T, sdpText string) (ufrag, fingerprint string) {
	t.Helper()
	var desc sdp.SessionDescription
	if err := desc.Unmarshal([]byte(sdpText)); err != nil {
		t.Fatalf("parse offer SDP: %v", err)
	}
	ufrag, _ = desc.Attribute("ice-ufrag")
	fingerprint, _ = desc.Attribute("fingerprint")
	for _, media := range desc.MediaDescriptions {
		if ufrag == "" {
			ufrag, _ = media.Attribute("ice-ufrag")
		}
		if fingerprint == "" {
			fingerprint, _ = media.Attribute("fingerprint")
		}
	}
	if ufrag == "" || fingerprint == "" {
		t.Fatal("offer is missing ICE ufrag or DTLS fingerprint")
	}
	return ufrag, fingerprint
}

func testRTPPacket(seq uint16) *rtp.Packet {
	return &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    96,
			SequenceNumber: seq,
			Timestamp:      uint32(seq) * 3000,
			SSRC:           0x1234,
		},
		Payload: []byte{0x01, 0x02, 0x03, 0x04},
	}
}

func TestVideoReceiveNewTransportPerWatch(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)

	first, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-one")
	if err != nil {
		t.Fatalf("first OpenReceive: %v", err)
	}
	if first.Nonce == "" {
		t.Fatal("first receive session has empty nonce")
	}
	ufrag1, fp1 := sdpTransportIdentity(t, first.SDP)
	if ufrag1 == "" || fp1 == "" {
		t.Fatalf("first offer missing transport identity (ufrag=%q fingerprint=%q)", ufrag1, fp1)
	}

	second, err := room.OpenReceive("viewer", "sharer", 2, "receive-session-two")
	if err != nil {
		t.Fatalf("second OpenReceive: %v", err)
	}
	if second.Nonce == first.Nonce {
		t.Fatal("new watch reused the old session nonce")
	}
	ufrag2, fp2 := sdpTransportIdentity(t, second.SDP)
	if ufrag2 == ufrag1 {
		t.Fatalf("new watch reused ICE ufrag %q; strict receive requires a new transport", ufrag1)
	}
	if fp2 == fp1 {
		t.Fatalf("new watch reused DTLS fingerprint %q; strict receive requires a new transport", fp1)
	}
	// 一次一路：旧会话必须已被摘除（旧 close/answer 不能落到新会话）。
	if _, ok := room.ReceiveSession(first.SessionID); ok {
		t.Fatal("old receive session still registered after a new watch")
	}
	if info, ok := room.ReceiveSession(second.SessionID); !ok || info.Viewer != "viewer" ||
		info.Target != "sharer" || info.Generation != 2 {
		t.Fatalf("second session info = %+v ok=%v", info, ok)
	}
}

func TestVideoReceiveStaleSignalRejected(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)

	offer, err := room.OpenReceive("viewer", "sharer", 7, "receive-session-stale")
	if err != nil {
		t.Fatalf("OpenReceive: %v", err)
	}
	answer := []byte(`{"type":"answer","sdp":"v=0\r\n"}`)
	if err := room.HandleReceiveSignal("viewer", offer.SessionID, "wrong-nonce", "answer", answer); err == nil {
		t.Fatal("answer with a wrong session nonce was accepted")
	}
	if err := room.HandleReceiveSignal("other-viewer", offer.SessionID, offer.Nonce, "answer", answer); err == nil {
		t.Fatal("answer from another viewer was accepted")
	}
	if !room.CloseReceive("viewer", offer.SessionID) {
		t.Fatal("CloseReceive returned false for the active session")
	}
	if err := room.HandleReceiveSignal("viewer", offer.SessionID, offer.Nonce, "answer", answer); err == nil {
		t.Fatal("answer for a closed session was accepted (must not reach a new receiver)")
	}
	// 旧 session 的 close 不能误关新会话。
	if _, err := room.OpenReceive("viewer", "sharer", 8, "receive-session-current"); err != nil {
		t.Fatalf("open new session: %v", err)
	}
	if room.CloseReceive("viewer", offer.SessionID) {
		t.Fatal("closing the stale session id reported a close of the current session")
	}
	if _, ok := room.ReceiveSession("receive-session-current"); !ok {
		t.Fatal("stale close removed the current receive session")
	}
}

func TestVideoReceiveCloseKeepsPublication(t *testing.T) {
	room := newReceiveTestRoom(t)
	track := addFakePublication(t, room, "sharer", 0)

	offer, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-publish")
	if err != nil {
		t.Fatalf("OpenReceive: %v", err)
	}
	if !room.CloseReceive("viewer", offer.SessionID) {
		t.Fatal("CloseReceive returned false")
	}
	// 发布状态/发布轨不能被接收侧的关闭影响。
	pubs := room.Publishers()
	if len(pubs) != 1 || pubs[0] != "sharer" {
		t.Fatalf("publishers after closing receive = %v, want [sharer]", pubs)
	}
	// 发布轨仍可写（StopVideoShare/ReleaseStreamObjects 没有被调用）。
	if err := track.WriteRTP(testRTPPacket(1)); err != nil {
		t.Fatalf("publication track became unusable after receive close: %v", err)
	}
	if got := room.ReceivePacketsWritten(offer.SessionID); got != 0 {
		t.Fatalf("closed session counted %d packet(s), want 0", got)
	}
}

func TestVideoReceiveForwardsOnlyOwnSession(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)

	first, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-a")
	if err != nil {
		t.Fatalf("OpenReceive A: %v", err)
	}
	room.writeToReceivesLocked("sharer", testRTPPacket(1))
	if got := room.ReceivePacketsWritten(first.SessionID); got != 1 {
		t.Fatalf("session A packets = %d, want 1", got)
	}
	if !room.CloseReceive("viewer", first.SessionID) {
		t.Fatal("close A")
	}

	second, err := room.OpenReceive("viewer", "sharer", 2, "receive-session-b")
	if err != nil {
		t.Fatalf("OpenReceive B: %v", err)
	}
	room.writeToReceivesLocked("sharer", testRTPPacket(2))
	if got := room.ReceivePacketsWritten(second.SessionID); got != 1 {
		t.Fatalf("session B packets = %d, want 1 (closed session A must not receive)", got)
	}
	// 关闭 A 之后 A 的计数不再增长（它已从 receives 摘除）。
	if _, ok := room.ReceiveSession(first.SessionID); ok {
		t.Fatal("closed session A is still registered")
	}
	if got := room.activeReceiveCount(); got != 1 {
		t.Fatalf("active receive sessions = %d, want 1", got)
	}
}

func TestVideoReceiveStoppedWithShare(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)
	offer, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-stop")
	if err != nil {
		t.Fatalf("OpenReceive: %v", err)
	}
	room.StopShare("sharer")
	if _, ok := room.ReceiveSession(offer.SessionID); ok {
		t.Fatal("stopping the share left a receive session alive")
	}
	if len(room.Publishers()) != 0 {
		t.Fatalf("publishers after StopShare = %v, want empty", room.Publishers())
	}
}

// TestVideoReceiveDuplicateSessionIDIsNotOverwritten 锁定 session_id 复用的生命周期：
// 旧实现遇到同一 sessionID 时会直接覆盖 r.receives[sessionID]，旧 PC 既没有关闭也
// 没有句柄（泄漏），且旧 nonce 被悄悄换掉。合同要求：未关闭的同一 sessionID 必须
// 幂等（同 tuple 返回同一会话）或明确拒绝（tuple 不同），不能隐式覆盖。
func TestVideoReceiveDuplicateSessionIDIsNotOverwritten(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)

	first, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-dup")
	if err != nil {
		t.Fatalf("OpenReceive: %v", err)
	}
	room.mu.Lock()
	firstPC := room.receives["receive-session-dup"].pc
	room.mu.Unlock()

	// 同 tuple 重试：必须返回同一个会话（同 nonce），不能新建第二条 PC。
	again, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-dup")
	if err != nil {
		t.Fatalf("idempotent OpenReceive returned error: %v", err)
	}
	if again.Nonce != first.Nonce || again.SessionID != first.SessionID {
		t.Fatalf("duplicate session id was replaced: nonce %q -> %q", first.Nonce, again.Nonce)
	}
	if got := room.activeReceiveCount(); got != 1 {
		t.Fatalf("active receive sessions after duplicate request = %d, want 1", got)
	}
	if firstPC.ConnectionState() == webrtc.PeerConnectionStateClosed {
		t.Fatal("idempotent retry closed the live receive PC")
	}

	// 同 ID 但 tuple 不同（换目标/换代次）：明确拒绝，原会话原样保留。
	if _, err := room.OpenReceive("viewer", "sharer", 2, "receive-session-dup"); err == nil {
		t.Fatal("same session_id with a new generation was accepted")
	}
	if _, err := room.OpenReceive("viewer", "other-sharer", 1, "receive-session-dup"); err == nil {
		t.Fatal("same session_id with another target was accepted")
	}
	if _, err := room.OpenReceive("other-viewer", "sharer", 1, "receive-session-dup"); err == nil {
		t.Fatal("same session_id for another viewer was accepted")
	}
	info, ok := room.ReceiveSession("receive-session-dup")
	if !ok || info.Viewer != "viewer" || info.Target != "sharer" || info.Generation != 1 {
		t.Fatalf("original session was modified by rejected duplicates: %+v ok=%v", info, ok)
	}
	if got := room.activeReceiveCount(); got != 1 {
		t.Fatalf("active receive sessions after rejected duplicates = %d, want 1", got)
	}
}

// TestVideoReceiveReplacementClosesOldPCOnce 锁定换台的顺序：新接收传输建成并登记后，
// 旧会话才被摘除/关闭；旧 PC 恰好关闭一次，receiveByViewer 指向新会话。
func TestVideoReceiveReplacementClosesOldPCOnce(t *testing.T) {
	room := newReceiveTestRoom(t)
	addFakePublication(t, room, "sharer", 0)

	first, err := room.OpenReceive("viewer", "sharer", 1, "receive-session-first")
	if err != nil {
		t.Fatalf("first OpenReceive: %v", err)
	}
	room.mu.Lock()
	oldPC := room.receives[first.SessionID].pc
	room.mu.Unlock()

	second, err := room.OpenReceive("viewer", "sharer2", 2, "receive-session-second")
	if err != nil {
		t.Fatalf("second OpenReceive: %v", err)
	}
	if oldPC.ConnectionState() != webrtc.PeerConnectionStateClosed {
		t.Fatalf("old receive PC state = %s after replacement, want closed",
			oldPC.ConnectionState())
	}
	if _, ok := room.ReceiveSession(first.SessionID); ok {
		t.Fatal("replaced session is still registered")
	}
	room.mu.Lock()
	current := room.receiveByViewer["viewer"]
	room.mu.Unlock()
	if current != second.SessionID {
		t.Fatalf("receiveByViewer = %q, want %q", current, second.SessionID)
	}
	if got := room.activeReceiveCount(); got != 1 {
		t.Fatalf("active receive sessions after replacement = %d, want 1", got)
	}
	// 旧 ID 的 close 不能误关当前会话，也不能 panic（PC 已经关过一次）。
	if room.CloseReceive("viewer", first.SessionID) {
		t.Fatal("closing the replaced session id reported success")
	}
	if _, ok := room.ReceiveSession(second.SessionID); !ok {
		t.Fatal("stale close removed the current receive session")
	}
}

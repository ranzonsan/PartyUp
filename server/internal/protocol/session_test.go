package protocol

// 会话替换通知（session.replaced）的线格式回归：客户端按字符串解析，字段名/取值
// 是协议的一部分，不能「反正 Go 侧能解」就随意改。
//
// 帧形如：
//
//	{"v":1,"id":"01JC...","type":"session.replaced","ts":1730000000000,
//	 "payload":{"reason":"login_elsewhere"}}

import (
	"encoding/json"
	"testing"
)

func TestSessionReplacedFrameFormat(t *testing.T) {
	if TypeSessionReplaced != "session.replaced" {
		t.Fatalf("TypeSessionReplaced = %q, want session.replaced", TypeSessionReplaced)
	}
	if SessionReasonLoginElsewhere != "login_elsewhere" {
		t.Fatalf("SessionReasonLoginElsewhere = %q, want login_elsewhere", SessionReasonLoginElsewhere)
	}
	if SessionReasonLogout != "logout" {
		t.Fatalf("SessionReasonLogout = %q, want logout", SessionReasonLogout)
	}
	if CodeSessionReplaced != "SESSION_REPLACED" {
		t.Fatalf("CodeSessionReplaced = %q, want SESSION_REPLACED", CodeSessionReplaced)
	}
	if CloseSessionReplaced != 4001 {
		t.Fatalf("CloseSessionReplaced = %d, want 4001（4000–4999 应用私有段）", CloseSessionReplaced)
	}

	env := MustNew(TypeSessionReplaced, SessionReplacedPayload{Reason: SessionReasonLoginElsewhere})
	raw, err := env.Marshal()
	if err != nil {
		t.Fatalf("marshal session.replaced: %v", err)
	}

	// 按客户端视角解析线格式：键名必须与 PROTOCOL.md §1.2 一致。
	var wire struct {
		V       int             `json:"v"`
		ID      string          `json:"id"`
		Type    string          `json:"type"`
		TS      int64           `json:"ts"`
		Payload json.RawMessage `json:"payload"`
	}
	if err := json.Unmarshal(raw, &wire); err != nil {
		t.Fatalf("unmarshal wire frame: %v", err)
	}
	if wire.V != Version {
		t.Fatalf("v = %d, want %d", wire.V, Version)
	}
	if wire.Type != "session.replaced" {
		t.Fatalf("type = %q, want session.replaced", wire.Type)
	}
	if wire.ID == "" || len(wire.ID) != 26 { // ULID = 26 个 Crockford base32 字符
		t.Fatalf("id = %q, want 26-char ULID", wire.ID)
	}
	if wire.TS <= 0 {
		t.Fatalf("ts = %d, want positive epoch ms", wire.TS)
	}

	var payload struct {
		Reason string `json:"reason"`
	}
	if err := json.Unmarshal(wire.Payload, &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload.Reason != "login_elsewhere" {
		t.Fatalf("payload.reason = %q, want login_elsewhere", payload.Reason)
	}

	// 服务端解码路径同样要认这条消息（测试与诊断工具会复用 Decode）。
	decoded, err := Decode(raw)
	if err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if decoded.Type != TypeSessionReplaced {
		t.Fatalf("decoded type = %q", decoded.Type)
	}
	var decodedPayload SessionReplacedPayload
	if err := decoded.DecodePayload(&decodedPayload); err != nil {
		t.Fatalf("decode payload: %v", err)
	}
	if decodedPayload.Reason != SessionReasonLoginElsewhere {
		t.Fatalf("decoded reason = %q", decodedPayload.Reason)
	}
}

// TestSessionReasonCloseCodes 锁定「termination reason ↔ WebSocket 关闭码」的映射
// （2026-11-07 小批次）：login_elsewhere=4001（batch 1 语义不变）、kicked=4002、
// banned=4003。客户端按 reason 出文案、按码兜底，两者都不能漂移。
func TestSessionReasonCloseCodes(t *testing.T) {
	cases := []struct {
		reason string
		want   int
	}{
		{SessionReasonLoginElsewhere, 4001},
		{SessionReasonKicked, 4002},
		{SessionReasonBanned, 4003},
		{SessionReasonLogout, 4001}, // 主动退出沿用 4001：客户端已在退出流程里
		{"unknown_reason", 4001},    // 未知 reason 不得让旧客户端拿到不认识的码
	}
	for _, tc := range cases {
		if got := CloseCodeForSessionReason(tc.reason); got != tc.want {
			t.Fatalf("CloseCodeForSessionReason(%q) = %d, want %d", tc.reason, got, tc.want)
		}
	}
	if SessionReasonKicked != "kicked" || SessionReasonBanned != "banned" {
		t.Fatalf("reason 字面量漂移: kicked=%q banned=%q", SessionReasonKicked, SessionReasonBanned)
	}
	if CloseSessionKicked != 4002 || CloseSessionBanned != 4003 {
		t.Fatalf("关闭码漂移: kicked=%d banned=%d", CloseSessionKicked, CloseSessionBanned)
	}
}
